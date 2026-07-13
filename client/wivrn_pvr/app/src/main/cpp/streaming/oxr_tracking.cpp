#include "oxr_tracking.h"

void oxr_tracker::set_head_pose_from_xr(const XrPosef & pose)
{
	float orient[4] = {
		pose.orientation.x,
		pose.orientation.y,
		pose.orientation.z,
		pose.orientation.w
	};
	float pos[3] = {
		pose.position.x,
		pose.position.y,
		pose.position.z
	};
	set_head_pose(orient, pos, nullptr, nullptr);
}
