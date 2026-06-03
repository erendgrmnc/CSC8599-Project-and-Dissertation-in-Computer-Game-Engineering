#pragma once
#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "OGLRenderer.h"

namespace NCL {
	namespace CSC8503 { class GameWorld; }
	namespace Rendering { class OGLShader; class OGLMesh; class Mesh; class Shader; }

	// Minimal, safe forward renderer for the distributed client. Draws every
	// GameObject in the world as a flat-shaded cube using one trivial shader and a
	// single shared mesh - no textures, no bindless handles, no deferred pipeline -
	// so it cannot issue the kind of invalid GPU work that hangs the display (the
	// failure mode of the full GameTechRenderer when used without a level).
	class DistributedClientRenderer : public OGLRenderer {
	public:
		DistributedClientRenderer(Window& w, NCL::CSC8503::GameWorld& world);
		~DistributedClientRenderer();

		// The shared cube mesh / shader, handed to the scene so its spawned
		// RenderObjects reference valid resources (their colour is what we draw).
		NCL::Rendering::Mesh* GetObjectMesh() const;
		NCL::Rendering::Shader* GetObjectShader() const;

	protected:
		void RenderFrame() override;

		NCL::CSC8503::GameWorld& mWorld;
		NCL::Rendering::OGLShader* mShader = nullptr;
		NCL::Rendering::OGLMesh* mCubeMesh = nullptr;
	};
}
#endif
