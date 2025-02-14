// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences.datareduction;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Build;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.LinearLayout.LayoutParams;

import org.chromium.base.ContextUtils;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.multiwindow.MultiWindowUtils;
import org.chromium.chrome.browser.net.spdyproxy.DataReductionProxySettings;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.browser.preferences.PrefServiceBridge.AboutVersionStrings;
import org.chromium.ui.widget.Toast;

/**
 * The promo screen encouraging users to enable Data Saver.
 */
public class DataReductionPromoScreen extends Dialog implements View.OnClickListener,
        DialogInterface.OnDismissListener {
    /**
     * Key used to save whether the promo screen is shown and the time in milliseconds since epoch,
     * it was shown.
     */
    private static final String SHARED_PREF_DISPLAYED_PROMO = "displayed_data_reduction_promo";
    private static final String SHARED_PREF_DISPLAYED_PROMO_TIME_MS =
            "displayed_data_reduction_promo_time_ms";
    private static final String SHARED_PREF_DISPLAYED_PROMO_VERSION =
            "displayed_data_reduction_promo_version";
    private static final String SHARED_PREF_FRE_PROMO_OPT_OUT = "fre_promo_opt_out";

    private int mState;

    private static View getContentView(Context context) {
        LayoutInflater inflater = (LayoutInflater) context.getSystemService(
                Context.LAYOUT_INFLATER_SERVICE);
        return inflater.inflate(R.layout.data_reduction_promo_screen, null);
    }

    /**
     * Launch the data reduction promo, if it needs to be displayed.
     */
    public static void launchDataReductionPromo(Activity parentActivity) {
        // The promo is displayed if Chrome is launched directly (i.e., not with the intent to
        // navigate to and view a URL on startup), the instance is part of the field trial,
        // and the promo has not been displayed before.
        if (!DataReductionProxySettings.getInstance().isDataReductionProxyPromoAllowed()) {
            return;
        }
        if (DataReductionProxySettings.getInstance().isDataReductionProxyManaged()) return;
        if (DataReductionProxySettings.getInstance().isDataReductionProxyEnabled()) return;
        if (getDisplayedDataReductionPromo(parentActivity)) return;
        // Showing the promo dialog in multiwindow mode is broken on Galaxy Note devices:
        // http://crbug.com/354696. If we're in multiwindow mode, save the dialog for later.
        if (MultiWindowUtils.getInstance().isLegacyMultiWindow(parentActivity)) return;

        DataReductionPromoScreen promoScreen = new DataReductionPromoScreen(parentActivity);
        promoScreen.setOnDismissListener(promoScreen);
        promoScreen.show();
    }

    /**
     * DataReductionPromoScreen constructor.
     *
     * @param context An Android context.
     */
    public DataReductionPromoScreen(Context context) {
        super(context, R.style.DataReductionPromoScreenDialog);
        setContentView(getContentView(context), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));

        // Remove the shadow from the enable button.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            Button enableButton = (Button) findViewById(R.id.enable_button);
            enableButton.setStateListAnimator(null);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Keep the window full screen otherwise the flip animation will frame-skip.
        getWindow().setLayout(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT);

        addListenerOnButton();

        mState = DataReductionProxyUma.ACTION_DISMISSED;
    }

    private void addListenerOnButton() {
        int [] interactiveViewIds = new int[] {
            R.id.no_thanks_button,
            R.id.enable_button,
            R.id.close_button
        };

        for (int interactiveViewId : interactiveViewIds) {
            findViewById(interactiveViewId).setOnClickListener(this);
        }
    }


    @Override
    public void onClick(View v) {
        int id = v.getId();

        if (id == R.id.no_thanks_button) {
            handleCloseButtonPressed();
        } else if (id == R.id.enable_button) {
            mState = DataReductionProxyUma.ACTION_ENABLED;
            handleEnableButtonPressed();
        } else if (id == R.id.close_button) {
            handleCloseButtonPressed();
        } else {
            assert false : "Unhandled onClick event";
        }
    }

    @Override
    public void onDismiss(DialogInterface dialog) {
        saveDataReductionPromoDisplayed(getContext());
    }

    private void handleEnableButtonPressed() {
        DataReductionProxySettings.getInstance().setDataReductionProxyEnabled(
                getContext(), true);
        dismiss();
        Toast.makeText(getContext(), getContext().getString(R.string.data_reduction_enabled_toast),
                Toast.LENGTH_LONG).show();
    }

    private void handleCloseButtonPressed() {
        dismiss();
    }

    @Override
    public void dismiss() {
        if (mState < DataReductionProxyUma.ACTION_INDEX_BOUNDARY) {
            DataReductionProxyUma.dataReductionProxyUIAction(mState);
            mState = DataReductionProxyUma.ACTION_INDEX_BOUNDARY;
        }
        super.dismiss();
    }

    /**
     * Returns whether the Data Reduction Proxy promo has been displayed before.
     *
     * @param context An Android context.
     * @return Whether the Data Reduction Proxy promo has been displayed.
     */
    public static boolean getDisplayedDataReductionPromo(Context context) {
        return ContextUtils.getAppSharedPreferences().getBoolean(
                SHARED_PREF_DISPLAYED_PROMO, false);
    }

    /**
     * Saves shared prefs indicating that the Data Reduction Proxy promo screen has been displayed
     * at the current time.
     *
     * @param context An Android context.
     */
    public static void saveDataReductionPromoDisplayed(Context context) {
        AboutVersionStrings versionStrings =
                PrefServiceBridge.getInstance().getAboutVersionStrings();
        ContextUtils.getAppSharedPreferences()
                .edit()
                .putBoolean(SHARED_PREF_DISPLAYED_PROMO, true)
                .putLong(SHARED_PREF_DISPLAYED_PROMO_TIME_MS, System.currentTimeMillis())
                .putString(SHARED_PREF_DISPLAYED_PROMO_VERSION,
                           versionStrings.getApplicationVersion())
                .apply();
    }

    /**
     * Saves shared prefs indicating that the Data Reduction Proxy First Run Experience promo screen
     * was displayed and the user opted out.
     *
     * @param context An Android context.
     * @param boolean Whether the user opted out of using the Data Reduction Proxy.
     */
    public static void saveDataReductionFrePromoOptOut(Context context, boolean optOut) {
        ContextUtils.getAppSharedPreferences()
                .edit()
                .putBoolean(SHARED_PREF_FRE_PROMO_OPT_OUT, optOut)
                .apply();
    }
}
