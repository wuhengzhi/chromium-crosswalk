// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VRController_h
#define VRController_h


#include "core/frame/LocalFrameLifecycleObserver.h"
#include "modules/ModulesExport.h"
#include "platform/Supplementable.h"
#include "public/platform/modules/vr/vr_service.mojom-blink.h"
#include "wtf/Deque.h"

#include <memory>

namespace blink {

class ServiceRegistry;
class VRGetDevicesCallback;

class MODULES_EXPORT VRController final
    : public GarbageCollectedFinalized<VRController>
    , public Supplement<LocalFrame>
    , public LocalFrameLifecycleObserver {
    USING_GARBAGE_COLLECTED_MIXIN(VRController);
    WTF_MAKE_NONCOPYABLE(VRController);
public:
    virtual ~VRController();

    void getDevices(std::unique_ptr<VRGetDevicesCallback>);

    mojom::blink::VRSensorStatePtr getSensorState(unsigned index);

    void resetSensor(unsigned index);

    static void provideTo(LocalFrame&, ServiceRegistry*);
    static VRController* from(LocalFrame&);
    static const char* supplementName();

    DECLARE_VIRTUAL_TRACE();

private:
    VRController(LocalFrame&, ServiceRegistry*);

    // Inherited from LocalFrameLifecycleObserver.
    void willDetachFrameHost() override;

    // Binding callbacks.
    void OnGetDevices(mojo::WTFArray<mojom::blink::VRDeviceInfoPtr>);

    Deque<std::unique_ptr<VRGetDevicesCallback>> m_pendingGetDevicesCallbacks;
    mojom::blink::VRServicePtr m_service;
};

} // namespace blink

#endif // VRController_h
