// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.net;

import org.chromium.base.Log;
import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.base.annotations.NativeClassQualifiedName;

import java.nio.ByteBuffer;
import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

import javax.annotation.concurrent.GuardedBy;

/**
 * {@link BidirectionalStream} implementation using Chromium network stack.
 * All @CalledByNative methods are called on the native network thread
 * and post tasks with callback calls onto Executor. Upon returning from callback, the native
 * stream is called on Executor thread and posts native tasks to the native network thread.
 */
@JNINamespace("cronet")
class CronetBidirectionalStream extends BidirectionalStream {
    /**
     * States of BidirectionalStream are tracked in mReadState and mWriteState.
     * The write state is separated out as it changes independently of the read state.
     * There is one initial state: State.NOT_STARTED. There is one normal final state:
     * State.SUCCESS, reached after State.READING_DONE and State.WRITING_DONE. There are two
     * exceptional final states: State.CANCELED and State.ERROR, which can be reached from
     * any other non-final state.
     */
    private enum State {
        /* Initial state, stream not started. */
        NOT_STARTED,
        /*
         * Stream started, request headers are being sent if mDelayRequestHeadersUntilNextFlush
         * is not set to true.
         */
        STARTED,
        /* Waiting for {@code read()} to be called. */
        WAITING_FOR_READ,
        /* Reading from the remote, {@code onReadCompleted()} callback will be called when done. */
        READING,
        /* There is no more data to read and stream is half-closed by the remote side. */
        READING_DONE,
        /* Stream is canceled. */
        CANCELED,
        /* Error has occured, stream is closed. */
        ERROR,
        /* Reading and writing are done, and the stream is closed successfully. */
        SUCCESS,
        /* Waiting for {@code nativeSendRequestHeaders()} or {@code nativeWritevData()} to be
           called. */
        WAITING_FOR_FLUSH,
        /* Writing to the remote, {@code onWritevCompleted()} callback will be called when done. */
        WRITING,
        /* There is no more data to write and stream is half-closed by the local side. */
        WRITING_DONE,
    }

    private final CronetUrlRequestContext mRequestContext;
    private final Executor mExecutor;
    private final Callback mCallback;
    private final String mInitialUrl;
    private final int mInitialPriority;
    private final String mInitialMethod;
    private final String mRequestHeaders[];
    private final boolean mDisableAutoFlush;
    private final boolean mDelayRequestHeadersUntilFirstFlush;

    /*
     * Synchronizes access to mNativeStream, mReadState and mWriteState.
     */
    private final Object mNativeStreamLock = new Object();

    @GuardedBy("mNativeStreamLock")
    // Pending write data.
    private LinkedList<ByteBuffer> mPendingData;

    @GuardedBy("mNativeStreamLock")
    // Flush data queue that should be pushed to the native stack when the previous
    // nativeWritevData completes.
    private LinkedList<ByteBuffer> mFlushData;

    @GuardedBy("mNativeStreamLock")
    // Whether an end-of-stream flag is passed in through write().
    private boolean mEndOfStreamWritten;

    @GuardedBy("mNativeStreamLock")
    // Whether request headers have been sent.
    private boolean mRequestHeadersSent;

    /* Native BidirectionalStream object, owned by CronetBidirectionalStream. */
    @GuardedBy("mNativeStreamLock")
    private long mNativeStream;

    /**
     * Read state is tracking reading flow.
     *                         / <--- READING <--- \
     *                         |                   |
     *                         \                   /
     * NOT_STARTED -> STARTED --> WAITING_FOR_READ -> READING_DONE -> SUCCESS
     */
    @GuardedBy("mNativeStreamLock")
    private State mReadState = State.NOT_STARTED;

    /**
     * Write state is tracking writing flow.
     *                         / <---  WRITING  <--- \
     *                         |                     |
     *                         \                     /
     * NOT_STARTED -> STARTED --> WAITING_FOR_FLUSH -> WRITING_DONE -> SUCCESS
     */
    @GuardedBy("mNativeStreamLock")
    private State mWriteState = State.NOT_STARTED;

    // Only modified on the network thread.
    private UrlResponseInfo mResponseInfo;

    /*
     * OnReadCompleted callback is repeatedly invoked when each read is completed, so it
     * is cached as a member variable.
     */
    // Only modified on the network thread.
    private OnReadCompletedRunnable mOnReadCompletedTask;

    private Runnable mOnDestroyedCallbackForTesting;

    private final class OnReadCompletedRunnable implements Runnable {
        // Buffer passed back from current invocation of onReadCompleted.
        ByteBuffer mByteBuffer;
        // End of stream flag from current invocation of onReadCompleted.
        boolean mEndOfStream;

        @Override
        public void run() {
            try {
                // Null out mByteBuffer, to pass buffer ownership to callback or release if done.
                ByteBuffer buffer = mByteBuffer;
                mByteBuffer = null;
                boolean maybeOnSucceeded = false;
                synchronized (mNativeStreamLock) {
                    if (isDoneLocked()) {
                        return;
                    }
                    if (mEndOfStream) {
                        mReadState = State.READING_DONE;
                        maybeOnSucceeded = (mWriteState == State.WRITING_DONE);
                    } else {
                        mReadState = State.WAITING_FOR_READ;
                    }
                }
                mCallback.onReadCompleted(
                        CronetBidirectionalStream.this, mResponseInfo, buffer, mEndOfStream);
                if (maybeOnSucceeded) {
                    maybeOnSucceededOnExecutor();
                }
            } catch (Exception e) {
                onCallbackException(e);
            }
        }
    }

    private final class OnWriteCompletedRunnable implements Runnable {
        // Buffer passed back from current invocation of onWriteCompleted.
        private ByteBuffer mByteBuffer;
        // End of stream flag from current call to write.
        private final boolean mEndOfStream;

        OnWriteCompletedRunnable(ByteBuffer buffer, boolean endOfStream) {
            mByteBuffer = buffer;
            mEndOfStream = endOfStream;
        }

        @Override
        public void run() {
            try {
                // Null out mByteBuffer, to pass buffer ownership to callback or release if done.
                ByteBuffer buffer = mByteBuffer;
                mByteBuffer = null;
                boolean maybeOnSucceeded = false;
                synchronized (mNativeStreamLock) {
                    if (isDoneLocked()) {
                        return;
                    }
                    if (mEndOfStream) {
                        mWriteState = State.WRITING_DONE;
                        maybeOnSucceeded = (mReadState == State.READING_DONE);
                    }
                }
                mCallback.onWriteCompleted(
                        CronetBidirectionalStream.this, mResponseInfo, buffer, mEndOfStream);
                if (maybeOnSucceeded) {
                    maybeOnSucceededOnExecutor();
                }
            } catch (Exception e) {
                onCallbackException(e);
            }
        }
    }

    CronetBidirectionalStream(CronetUrlRequestContext requestContext, String url,
            @BidirectionalStream.Builder.StreamPriority int priority, Callback callback,
            Executor executor, String httpMethod, List<Map.Entry<String, String>> requestHeaders,
            boolean disableAutoFlush, boolean delayRequestHeadersUntilNextFlush) {
        mRequestContext = requestContext;
        mInitialUrl = url;
        mInitialPriority = convertStreamPriority(priority);
        mCallback = callback;
        mExecutor = executor;
        mInitialMethod = httpMethod;
        mRequestHeaders = stringsFromHeaderList(requestHeaders);
        mDisableAutoFlush = disableAutoFlush;
        mDelayRequestHeadersUntilFirstFlush = delayRequestHeadersUntilNextFlush;
        mPendingData = new LinkedList<>();
        mFlushData = new LinkedList<>();
    }

    @Override
    public void start() {
        synchronized (mNativeStreamLock) {
            if (mReadState != State.NOT_STARTED) {
                throw new IllegalStateException("Stream is already started.");
            }
            try {
                mNativeStream = nativeCreateBidirectionalStream(
                        mRequestContext.getUrlRequestContextAdapter(),
                        !mDelayRequestHeadersUntilFirstFlush);
                mRequestContext.onRequestStarted();
                // Non-zero startResult means an argument error.
                int startResult = nativeStart(mNativeStream, mInitialUrl, mInitialPriority,
                        mInitialMethod, mRequestHeaders, !doesMethodAllowWriteData(mInitialMethod));
                if (startResult == -1) {
                    throw new IllegalArgumentException("Invalid http method " + mInitialMethod);
                }
                if (startResult > 0) {
                    int headerPos = startResult - 1;
                    throw new IllegalArgumentException("Invalid header "
                            + mRequestHeaders[headerPos] + "=" + mRequestHeaders[headerPos + 1]);
                }
                mReadState = mWriteState = State.STARTED;
            } catch (RuntimeException e) {
                // If there's an exception, clean up and then throw the
                // exception to the caller.
                destroyNativeStreamLocked(false);
                throw e;
            }
        }
    }

    @Override
    public void read(ByteBuffer buffer) {
        synchronized (mNativeStreamLock) {
            Preconditions.checkHasRemaining(buffer);
            Preconditions.checkDirect(buffer);
            if (mReadState != State.WAITING_FOR_READ) {
                throw new IllegalStateException("Unexpected read attempt.");
            }
            if (isDoneLocked()) {
                return;
            }
            if (mOnReadCompletedTask == null) {
                mOnReadCompletedTask = new OnReadCompletedRunnable();
            }
            mReadState = State.READING;
            if (!nativeReadData(mNativeStream, buffer, buffer.position(), buffer.limit())) {
                // Still waiting on read. This is just to have consistent
                // behavior with the other error cases.
                mReadState = State.WAITING_FOR_READ;
                throw new IllegalArgumentException("Unable to call native read");
            }
        }
    }

    @Override
    public void write(ByteBuffer buffer, boolean endOfStream) {
        synchronized (mNativeStreamLock) {
            Preconditions.checkDirect(buffer);
            if (!buffer.hasRemaining() && !endOfStream) {
                throw new IllegalArgumentException("Empty buffer before end of stream.");
            }
            if (mEndOfStreamWritten) {
                throw new IllegalArgumentException("Write after writing end of stream.");
            }
            if (isDoneLocked()) {
                return;
            }
            mPendingData.add(buffer);
            if (endOfStream) {
                mEndOfStreamWritten = true;
            }
            if (!mDisableAutoFlush) {
                flushLocked();
            }
        }
    }

    @Override
    public void flush() {
        synchronized (mNativeStreamLock) {
            flushLocked();
        }
    }

    @SuppressWarnings("GuardedByChecker")
    private void flushLocked() {
        if (isDoneLocked()
                || (mWriteState != State.WAITING_FOR_FLUSH && mWriteState != State.WRITING)) {
            return;
        }
        if (mPendingData.isEmpty() && mFlushData.isEmpty()) {
            // If there is no pending write when flush() is called, see if
            // request headers need to be flushed.
            if (!mRequestHeadersSent) {
                mRequestHeadersSent = true;
                nativeSendRequestHeaders(mNativeStream);
                if (!doesMethodAllowWriteData(mInitialMethod)) {
                    mWriteState = State.WRITING_DONE;
                }
            }
            return;
        }

        assert !mPendingData.isEmpty() || !mFlushData.isEmpty();

        // Move buffers from mPendingData to the flushing queue.
        if (!mPendingData.isEmpty()) {
            mFlushData.addAll(mPendingData);
            mPendingData.clear();
        }

        if (mWriteState == State.WRITING) {
            // If there is a write already pending, wait until onWritevCompleted is
            // called before pushing data to the native stack.
            return;
        }
        assert mWriteState == State.WAITING_FOR_FLUSH;
        int size = mFlushData.size();
        ByteBuffer[] buffers = new ByteBuffer[size];
        int[] positions = new int[size];
        int[] limits = new int[size];
        for (int i = 0; i < size; i++) {
            ByteBuffer buffer = mFlushData.poll();
            buffers[i] = buffer;
            positions[i] = buffer.position();
            limits[i] = buffer.limit();
        }
        assert mFlushData.isEmpty();
        assert buffers.length >= 1;
        mWriteState = State.WRITING;
        if (!nativeWritevData(mNativeStream, buffers, positions, limits, mEndOfStreamWritten)) {
            // Still waiting on flush. This is just to have consistent
            // behavior with the other error cases.
            mWriteState = State.WAITING_FOR_FLUSH;
            throw new IllegalArgumentException("Unable to call native writev.");
        }
    }

    @Override
    public void ping(PingCallback callback, Executor executor) {
        // TODO(mef): May be last thing to be implemented on Android.
        throw new UnsupportedOperationException("ping is not supported yet.");
    }

    @Override
    public void windowUpdate(int windowSizeIncrement) {
        // TODO(mef): Understand the needs and semantics of this method.
        throw new UnsupportedOperationException("windowUpdate is not supported yet.");
    }

    @Override
    public void cancel() {
        synchronized (mNativeStreamLock) {
            if (isDoneLocked() || mReadState == State.NOT_STARTED) {
                return;
            }
            mReadState = mWriteState = State.CANCELED;
            destroyNativeStreamLocked(true);
        }
    }

    @Override
    public boolean isDone() {
        synchronized (mNativeStreamLock) {
            return isDoneLocked();
        }
    }

    @GuardedBy("mNativeStreamLock")
    private boolean isDoneLocked() {
        return mReadState != State.NOT_STARTED && mNativeStream == 0;
    }

    /*
     * Runs an onSucceeded callback if both Read and Write sides are closed.
     */
    private void maybeOnSucceededOnExecutor() {
        synchronized (mNativeStreamLock) {
            if (isDoneLocked()) {
                return;
            }
            if (!(mWriteState == State.WRITING_DONE && mReadState == State.READING_DONE)) {
                return;
            }
            mReadState = mWriteState = State.SUCCESS;
            // Destroy native stream first, so UrlRequestContext could be shut
            // down from the listener.
            destroyNativeStreamLocked(false);
        }
        try {
            mCallback.onSucceeded(CronetBidirectionalStream.this, mResponseInfo);
        } catch (Exception e) {
            Log.e(CronetUrlRequestContext.LOG_TAG, "Exception in onSucceeded method", e);
        }
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onStreamReady(final boolean requestHeadersSent) {
        postTaskToExecutor(new Runnable() {
            public void run() {
                synchronized (mNativeStreamLock) {
                    if (isDoneLocked()) {
                        return;
                    }
                    mRequestHeadersSent = requestHeadersSent;
                    mReadState = State.WAITING_FOR_READ;
                    if (!doesMethodAllowWriteData(mInitialMethod) && mRequestHeadersSent) {
                        mWriteState = State.WRITING_DONE;
                    } else {
                        mWriteState = State.WAITING_FOR_FLUSH;
                    }
                }

                try {
                    mCallback.onStreamReady(CronetBidirectionalStream.this);
                } catch (Exception e) {
                    onCallbackException(e);
                }
            }
        });
    }

    /**
     * Called when the final set of headers, after all redirects,
     * is received. Can only be called once for each stream.
     */
    @SuppressWarnings("unused")
    @CalledByNative
    private void onResponseHeadersReceived(int httpStatusCode, String negotiatedProtocol,
            String[] headers, long receivedBytesCount) {
        try {
            mResponseInfo = prepareResponseInfoOnNetworkThread(
                    httpStatusCode, negotiatedProtocol, headers, receivedBytesCount);
        } catch (Exception e) {
            failWithException(new CronetException("Cannot prepare ResponseInfo", null));
            return;
        }
        postTaskToExecutor(new Runnable() {
            public void run() {
                synchronized (mNativeStreamLock) {
                    if (isDoneLocked()) {
                        return;
                    }
                    mReadState = State.WAITING_FOR_READ;
                }

                try {
                    mCallback.onResponseHeadersReceived(
                            CronetBidirectionalStream.this, mResponseInfo);
                } catch (Exception e) {
                    onCallbackException(e);
                }
            }
        });
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onReadCompleted(final ByteBuffer byteBuffer, int bytesRead, int initialPosition,
            int initialLimit, long receivedBytesCount) {
        mResponseInfo.setReceivedBytesCount(receivedBytesCount);
        if (byteBuffer.position() != initialPosition || byteBuffer.limit() != initialLimit) {
            failWithException(
                    new CronetException("ByteBuffer modified externally during read", null));
            return;
        }
        if (bytesRead < 0 || initialPosition + bytesRead > initialLimit) {
            failWithException(new CronetException("Invalid number of bytes read", null));
            return;
        }
        byteBuffer.position(initialPosition + bytesRead);
        assert mOnReadCompletedTask.mByteBuffer == null;
        mOnReadCompletedTask.mByteBuffer = byteBuffer;
        mOnReadCompletedTask.mEndOfStream = (bytesRead == 0);
        postTaskToExecutor(mOnReadCompletedTask);
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onWritevCompleted(final ByteBuffer[] byteBuffers, int[] initialPositions,
            int[] initialLimits, boolean endOfStream) {
        assert byteBuffers.length == initialPositions.length;
        assert byteBuffers.length == initialLimits.length;
        synchronized (mNativeStreamLock) {
            mWriteState = State.WAITING_FOR_FLUSH;
            // Flush if there is anything in the flush queue mFlushData.
            if (!mFlushData.isEmpty()) {
                flushLocked();
            }
        }
        for (int i = 0; i < byteBuffers.length; i++) {
            ByteBuffer buffer = byteBuffers[i];
            if (buffer.position() != initialPositions[i] || buffer.limit() != initialLimits[i]) {
                failWithException(
                        new CronetException("ByteBuffer modified externally during write", null));
                return;
            }
            // Current implementation always writes the complete buffer.
            buffer.position(buffer.limit());
            postTaskToExecutor(new OnWriteCompletedRunnable(buffer, endOfStream));
        }
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onResponseTrailersReceived(String[] trailers) {
        final UrlResponseInfo.HeaderBlock trailersBlock =
                new UrlResponseInfo.HeaderBlock(headersListFromStrings(trailers));
        postTaskToExecutor(new Runnable() {
            public void run() {
                synchronized (mNativeStreamLock) {
                    if (isDoneLocked()) {
                        return;
                    }
                }
                try {
                    mCallback.onResponseTrailersReceived(
                            CronetBidirectionalStream.this, mResponseInfo, trailersBlock);
                } catch (Exception e) {
                    onCallbackException(e);
                }
            }
        });
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onError(
            int errorCode, int nativeError, String errorString, long receivedBytesCount) {
        if (mResponseInfo != null) {
            mResponseInfo.setReceivedBytesCount(receivedBytesCount);
        }
        failWithException(new CronetException(
                "Exception in BidirectionalStream: " + errorString, errorCode, nativeError));
    }

    /**
     * Called when request is canceled, no callbacks will be called afterwards.
     */
    @SuppressWarnings("unused")
    @CalledByNative
    private void onCanceled() {
        postTaskToExecutor(new Runnable() {
            public void run() {
                try {
                    mCallback.onCanceled(CronetBidirectionalStream.this, mResponseInfo);
                } catch (Exception e) {
                    Log.e(CronetUrlRequestContext.LOG_TAG, "Exception in onCanceled method", e);
                }
            }
        });
    }

    @VisibleForTesting
    public void setOnDestroyedCallbackForTesting(Runnable onDestroyedCallbackForTesting) {
        mOnDestroyedCallbackForTesting = onDestroyedCallbackForTesting;
    }

    private static boolean doesMethodAllowWriteData(String methodName) {
        return !methodName.equals("GET") && !methodName.equals("HEAD");
    }

    private static ArrayList<Map.Entry<String, String>> headersListFromStrings(String[] headers) {
        ArrayList<Map.Entry<String, String>> headersList = new ArrayList<>(headers.length / 2);
        for (int i = 0; i < headers.length; i += 2) {
            headersList.add(new AbstractMap.SimpleImmutableEntry<>(headers[i], headers[i + 1]));
        }
        return headersList;
    }

    private static String[] stringsFromHeaderList(List<Map.Entry<String, String>> headersList) {
        String headersArray[] = new String[headersList.size() * 2];
        int i = 0;
        for (Map.Entry<String, String> requestHeader : headersList) {
            headersArray[i++] = requestHeader.getKey();
            headersArray[i++] = requestHeader.getValue();
        }
        return headersArray;
    }

    private static int convertStreamPriority(
            @BidirectionalStream.Builder.StreamPriority int priority) {
        switch (priority) {
            case Builder.STREAM_PRIORITY_IDLE:
                return RequestPriority.IDLE;
            case Builder.STREAM_PRIORITY_LOWEST:
                return RequestPriority.LOWEST;
            case Builder.STREAM_PRIORITY_LOW:
                return RequestPriority.LOW;
            case Builder.STREAM_PRIORITY_MEDIUM:
                return RequestPriority.MEDIUM;
            case Builder.STREAM_PRIORITY_HIGHEST:
                return RequestPriority.HIGHEST;
            default:
                throw new IllegalArgumentException("Invalid stream priority.");
        }
    }

    /**
     * Posts task to application Executor. Used for callbacks
     * and other tasks that should not be executed on network thread.
     */
    private void postTaskToExecutor(Runnable task) {
        try {
            mExecutor.execute(task);
        } catch (RejectedExecutionException failException) {
            Log.e(CronetUrlRequestContext.LOG_TAG, "Exception posting task to executor",
                    failException);
            // If posting a task throws an exception, then there is no choice
            // but to destroy the stream without invoking the callback.
            synchronized (mNativeStreamLock) {
                mReadState = mWriteState = State.ERROR;
                destroyNativeStreamLocked(false);
            }
        }
    }

    private UrlResponseInfo prepareResponseInfoOnNetworkThread(int httpStatusCode,
            String negotiatedProtocol, String[] headers, long receivedBytesCount) {
        UrlResponseInfo responseInfo =
                new UrlResponseInfo(Arrays.asList(mInitialUrl), httpStatusCode, "",
                        headersListFromStrings(headers), false, negotiatedProtocol, null);
        responseInfo.setReceivedBytesCount(receivedBytesCount);
        return responseInfo;
    }

    @GuardedBy("mNativeStreamLock")
    private void destroyNativeStreamLocked(boolean sendOnCanceled) {
        Log.i(CronetUrlRequestContext.LOG_TAG, "destroyNativeStreamLocked " + this.toString());
        if (mNativeStream == 0) {
            return;
        }
        nativeDestroy(mNativeStream, sendOnCanceled);
        mNativeStream = 0;
        mRequestContext.onRequestDestroyed();
        if (mOnDestroyedCallbackForTesting != null) {
            mOnDestroyedCallbackForTesting.run();
        }
    }

    /**
     * Fails the stream with an exception. Only called on the Executor.
     */
    private void failWithExceptionOnExecutor(CronetException e) {
        // Do not call into mCallback if request is complete.
        synchronized (mNativeStreamLock) {
            if (isDoneLocked()) {
                return;
            }
            mReadState = mWriteState = State.ERROR;
            destroyNativeStreamLocked(false);
        }
        try {
            mCallback.onFailed(this, mResponseInfo, e);
        } catch (Exception failException) {
            Log.e(CronetUrlRequestContext.LOG_TAG, "Exception notifying of failed request",
                    failException);
        }
    }

    /**
     * If callback method throws an exception, stream gets canceled
     * and exception is reported via onFailed callback.
     * Only called on the Executor.
     */
    private void onCallbackException(Exception e) {
        CronetException streamError =
                new CronetException("CalledByNative method has thrown an exception", e);
        Log.e(CronetUrlRequestContext.LOG_TAG, "Exception in CalledByNative method", e);
        failWithExceptionOnExecutor(streamError);
    }

    /**
     * Fails the stream with an exception. Can be called on any thread.
     */
    private void failWithException(final CronetException exception) {
        postTaskToExecutor(new Runnable() {
            public void run() {
                failWithExceptionOnExecutor(exception);
            }
        });
    }

    // Native methods are implemented in cronet_bidirectional_stream_adapter.cc.
    private native long nativeCreateBidirectionalStream(
            long urlRequestContextAdapter, boolean sendRequestHeadersAutomatically);

    @NativeClassQualifiedName("CronetBidirectionalStreamAdapter")
    private native int nativeStart(long nativePtr, String url, int priority, String method,
            String[] headers, boolean endOfStream);

    @NativeClassQualifiedName("CronetBidirectionalStreamAdapter")
    private native void nativeSendRequestHeaders(long nativePtr);

    @NativeClassQualifiedName("CronetBidirectionalStreamAdapter")
    private native boolean nativeReadData(
            long nativePtr, ByteBuffer byteBuffer, int position, int limit);

    @NativeClassQualifiedName("CronetBidirectionalStreamAdapter")
    private native boolean nativeWritevData(long nativePtr, ByteBuffer[] buffers, int[] positions,
            int[] limits, boolean endOfStream);

    @NativeClassQualifiedName("CronetBidirectionalStreamAdapter")
    private native void nativeDestroy(long nativePtr, boolean sendOnCanceled);
}
