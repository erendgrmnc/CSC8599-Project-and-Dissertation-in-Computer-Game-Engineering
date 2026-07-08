#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "DistributedClientOverlay.h"

#include "DistributedMultiplayerGameScene.h"   // ServerRegion
#include "Debug.h"

#include <string>

using namespace NCL;
using namespace NCL::Maths;

namespace {
	// Ground grid sits just above y=0 so it doesn't z-fight with objects resting there.
	constexpr float kGridY = 0.2f;
	constexpr int   kSubdivisions = 8;     // internal grid cells per region edge

	// Legend layout, in the 0..100 orthographic space the text shader uses (y=100 top).
	constexpr float kLegendX = 2.0f;
	constexpr float kLegendTopY = 96.0f;
	constexpr float kLegendLineStep = 3.5f;
	constexpr float kLegendFontSize = 20.0f;
}

void DistributedClientOverlay::Emit(const std::vector<ServerRegion>& regions, bool drawLegend) {
	for (const ServerRegion& r : regions) {
		const Vector4 c = r.colour;
		const Vector4 dim(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1.0f);

		// Region outline.
		const Vector3 a(r.minX, kGridY, r.minZ);
		const Vector3 b(r.maxX, kGridY, r.minZ);
		const Vector3 d(r.maxX, kGridY, r.maxZ);
		const Vector3 e(r.minX, kGridY, r.maxZ);
		Debug::DrawLine(a, b, c);
		Debug::DrawLine(b, d, c);
		Debug::DrawLine(d, e, c);
		Debug::DrawLine(e, a, c);

		// Internal grid, dimmer, so the region reads as a floor plane and objects can be
		// judged against it.
		for (int i = 1; i < kSubdivisions; ++i) {
			const float t = static_cast<float>(i) / kSubdivisions;
			const float x = r.minX + (r.maxX - r.minX) * t;
			const float z = r.minZ + (r.maxZ - r.minZ) * t;
			Debug::DrawLine(Vector3(x, kGridY, r.minZ), Vector3(x, kGridY, r.maxZ), dim);
			Debug::DrawLine(Vector3(r.minX, kGridY, z), Vector3(r.maxX, kGridY, z), dim);
		}
	}

	if (!drawLegend) {
		return;
	}

	float y = kLegendTopY;
	for (const ServerRegion& r : regions) {
		Debug::Print("Server " + std::to_string(r.serverId), Vector2(kLegendX, y), r.colour, kLegendFontSize);
		y -= kLegendLineStep;
	}
}
#endif
