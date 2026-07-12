#pragma once

#include "pico_tracking.h"
#include "wivrn_client_pico.h"

struct wivrn_session_pico;

// In pico_pvr, tracking comes directly from the PVR SDK via Pvr_GetMainSensorState.
// No OpenXR pose injection needed - the tracker reads from the SDK itself.
class pvr_tracker : public pico_native_tracker
{
public:
	// No additional methods needed - pico_native_tracker already reads from
	// Pvr_GetMainSensorState when no external pose is set.
};
