#pragma once
#ifndef DISTRIBUTEDSYSTEMACTIVE

#include <vector>

struct ServerRegion;

namespace NCL {
	// Emits the server-region visualisation as Debug primitives: a colour-coded ground
	// grid per region plus a top-left legend mapping colour -> Server N. The renderer
	// consumes these via Debug::GetDebugLines()/GetDebugStrings() the same frame, so
	// Emit must run each frame (while the overlay is enabled) before the render, and the
	// host must call Debug::UpdateRenderables afterwards to clear them.
	class DistributedClientOverlay {
	public:
		static void Emit(const std::vector<ServerRegion>& regions, bool drawLegend);
	};
}
#endif
