#pragma once
#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "OGLRenderer.h"

#include "Matrix4.h"
#include "Vector2.h"
#include "Vector3.h"
#include "Vector4.h"

#include <vector>

namespace NCL {
	namespace CSC8503 { class GameWorld; }
	namespace Rendering { class OGLShader; class OGLMesh; class OGLTexture; class Mesh; class Shader; }

	// Minimal, safe forward renderer for the distributed client. Draws every
	// GameObject in the world as a flat-shaded cube using one trivial shader and a
	// single shared mesh - no textures, no bindless handles, no deferred pipeline -
	// so it cannot issue the kind of invalid GPU work that hangs the display (the
	// failure mode of the full GameTechRenderer when used without a level).
	//
	// It also draws the Debug line/text primitives the region overlay emits, using
	// its own trivial line and text shaders (classic sampler2D, no bindless), so the
	// overlay stays within the same safe envelope.
	class DistributedClientRenderer : public OGLRenderer {
	public:
		DistributedClientRenderer(Window& w, NCL::CSC8503::GameWorld& world);
		~DistributedClientRenderer();

		// The shared cube mesh / shader, handed to the scene so its spawned
		// RenderObjects reference valid resources (their colour is what we draw).
		NCL::Rendering::Mesh* GetObjectMesh() const;
		NCL::Rendering::Shader* GetObjectShader() const;

		// True once the debug font atlas loaded; the overlay legend is suppressed
		// otherwise (grid + tint still work).
		bool HasDebugFont() const { return mFontLoaded; }

	protected:
		void RenderFrame() override;

		// Consume Debug::GetDebugLines()/GetDebugStrings() emitted this frame.
		void RenderOverlayLines();
		void RenderOverlayText();
		void SetLineBufferSizes(size_t entryCount);
		void SetTextBufferSizes(size_t vertCount);

		NCL::CSC8503::GameWorld& mWorld;
		NCL::Rendering::OGLShader* mShader = nullptr;
		NCL::Rendering::OGLMesh* mCubeMesh = nullptr;

		// Overlay: coloured region grid (lines) + legend (text).
		NCL::Rendering::OGLShader* mLineShader = nullptr;
		NCL::Rendering::OGLShader* mTextShader = nullptr;
		NCL::Rendering::OGLTexture* mFontTex = nullptr;
		bool mFontLoaded = false;

		GLuint mLineVAO = 0, mLineVBO = 0;
		GLuint mTextVAO = 0, mTextVertVBO = 0, mTextColourVBO = 0, mTextTexVBO = 0;
		size_t mLineCount = 0, mTextCount = 0;

		std::vector<NCL::Maths::Vector3> mTextPos;
		std::vector<NCL::Maths::Vector4> mTextColours;
		std::vector<NCL::Maths::Vector2> mTextUVs;
	};
}
#endif
