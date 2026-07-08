#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "DistributedClientRenderer.h"

#include "OGLShader.h"
#include "OGLMesh.h"
#include "OGLTexture.h"
#include "MshLoader.h"
#include "GameWorld.h"
#include "GameObject.h"
#include "RenderObject.h"
#include "Camera.h"
#include "Debug.h"
#include "SimpleFont.h"

#include <iostream>

using namespace NCL;
using namespace NCL::CSC8503;
using namespace NCL::Rendering;
using namespace NCL::Maths;

DistributedClientRenderer::DistributedClientRenderer(Window& w, GameWorld& world)
	: OGLRenderer(w), mWorld(world) {

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);

	mShader = new OGLShader("clientObject.vert", "clientObject.frag");

	mCubeMesh = new OGLMesh();
	MshLoader::LoadMesh("Cube.msh", *mCubeMesh);
	mCubeMesh->SetPrimitiveType(GeometryPrimitive::Triangles);
	mCubeMesh->UploadToGPU();

	// Overlay resources: trivial line + text shaders and their dynamic buffers.
	mLineShader = new OGLShader("overlayLine.vert", "overlayLine.frag");
	mTextShader = new OGLShader("overlayText.vert", "overlayText.frag");

	glGenVertexArrays(1, &mLineVAO);
	glGenBuffers(1, &mLineVBO);

	glGenVertexArrays(1, &mTextVAO);
	glGenBuffers(1, &mTextVertVBO);
	glGenBuffers(1, &mTextColourVBO);
	glGenBuffers(1, &mTextTexVBO);

	// Font atlas for the legend. If it fails to load, the overlay simply skips text;
	// the grid and object tints do not depend on it.
	try {
		mFontTex = OGLTexture::TextureFromFile("PressStart2P.png").release();
		if (mFontTex) {
			Debug::CreateDebugFont("PressStart2P.fnt", *mFontTex);
			mFontLoaded = Debug::GetDebugFont() != nullptr;
		}
	}
	catch (const std::exception& e) {
		std::cout << "Overlay font failed to load (" << e.what() << "); legend disabled.\n";
		mFontLoaded = false;
	}

	std::cout << "Distributed client forward renderer ready (overlay font "
		<< (mFontLoaded ? "loaded" : "MISSING") << ").\n";
}

DistributedClientRenderer::~DistributedClientRenderer() {
	delete mShader;
	delete mCubeMesh;
	delete mLineShader;
	delete mTextShader;
	delete mFontTex;

	glDeleteVertexArrays(1, &mLineVAO);
	glDeleteBuffers(1, &mLineVBO);
	glDeleteVertexArrays(1, &mTextVAO);
	glDeleteBuffers(1, &mTextVertVBO);
	glDeleteBuffers(1, &mTextColourVBO);
	glDeleteBuffers(1, &mTextTexVBO);
}

Mesh* DistributedClientRenderer::GetObjectMesh() const {
	return mCubeMesh;
}

Shader* DistributedClientRenderer::GetObjectShader() const {
	return mShader;
}

void DistributedClientRenderer::RenderFrame() {
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	Camera& cam = mWorld.GetMainCamera();
	Matrix4 viewProj = cam.BuildProjectionMatrix(hostWindow.GetScreenAspect()) * cam.BuildViewMatrix();

	BindShader(*mShader);
	const unsigned int prog = mShader->GetProgramID();
	const int modelLoc = glGetUniformLocation(prog, "modelMatrix");
	const int vpLoc = glGetUniformLocation(prog, "viewProjMatrix");
	const int colLoc = glGetUniformLocation(prog, "objectColour");
	glUniformMatrix4fv(vpLoc, 1, false, (const float*)&viewProj);

	BindMesh(*mCubeMesh);
	const unsigned int subCount = (unsigned int)mCubeMesh->GetSubMeshCount();

	mWorld.OperateOnContents([&](GameObject* o) {
		RenderObject* ro = o->GetRenderObject();
		if (!ro) {
			return;
		}
		Matrix4 model = o->GetTransform().GetMatrix();
		Vector4 colour = ro->GetColour();
		glUniformMatrix4fv(modelLoc, 1, false, (const float*)&model);
		glUniform4fv(colLoc, 1, (const float*)&colour);
		for (unsigned int s = 0; s < subCount; ++s) {
			DrawBoundMesh(s);
		}
	});

	// Region overlay on top (world-space grid, then screen-space legend).
	RenderOverlayLines();
	RenderOverlayText();
}

void DistributedClientRenderer::RenderOverlayLines() {
	const std::vector<Debug::DebugLineEntry>& lines = Debug::GetDebugLines();
	if (lines.empty()) {
		return;
	}

	Camera& cam = mWorld.GetMainCamera();
	Matrix4 viewProj = cam.BuildProjectionMatrix(hostWindow.GetScreenAspect()) * cam.BuildViewMatrix();

	BindShader(*mLineShader);
	glUniformMatrix4fv(glGetUniformLocation(mLineShader->GetProgramID(), "viewProjMatrix"),
		1, false, (const float*)&viewProj);

	const size_t vertCount = lines.size() * 2;
	SetLineBufferSizes(lines.size());

	glBindBuffer(GL_ARRAY_BUFFER, mLineVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, lines.size() * sizeof(Debug::DebugLineEntry), lines.data());

	glBindVertexArray(mLineVAO);
	glDrawArrays(GL_LINES, 0, (GLsizei)vertCount);
	glBindVertexArray(0);
}

void DistributedClientRenderer::RenderOverlayText() {
	const std::vector<Debug::DebugStringEntry>& strings = Debug::GetDebugStrings();
	if (strings.empty() || !mFontLoaded) {
		return;
	}

	BindShader(*mTextShader);

	// Same 0..100 orthographic space the engine's own debug text uses, so the
	// PressStart2P glyph sizes match what Debug::Print expects.
	Matrix4 orthProj = Matrix4::Orthographic(0.0f, 100.0f, 100.0f, 0.0f, -1.0f, 1.0f);
	glUniformMatrix4fv(glGetUniformLocation(mTextShader->GetProgramID(), "orthProj"),
		1, false, (const float*)&orthProj);

	// Classic single sampler bind - no bindless handles.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, mFontTex->GetObjectID());
	glUniform1i(glGetUniformLocation(mTextShader->GetProgramID(), "fontTex"), 0);

	mTextPos.clear();
	mTextColours.clear();
	mTextUVs.clear();

	int frameVertCount = 0;
	for (const auto& s : strings) {
		frameVertCount += Debug::GetDebugFont()->GetVertexCountForString(s.data);
	}
	SetTextBufferSizes(frameVertCount);

	for (const auto& s : strings) {
		Debug::GetDebugFont()->BuildVerticesForString(s.data, s.position, s.colour, s.fontSize,
			mTextPos, mTextUVs, mTextColours);
	}

	glBindBuffer(GL_ARRAY_BUFFER, mTextVertVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, frameVertCount * sizeof(Vector3), mTextPos.data());
	glBindBuffer(GL_ARRAY_BUFFER, mTextColourVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, frameVertCount * sizeof(Vector4), mTextColours.data());
	glBindBuffer(GL_ARRAY_BUFFER, mTextTexVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, frameVertCount * sizeof(Vector2), mTextUVs.data());

	// HUD text: blend for glyph edges, no depth test / cull so it always shows.
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glBindVertexArray(mTextVAO);
	glDrawArrays(GL_TRIANGLES, 0, frameVertCount);
	glBindVertexArray(0);

	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
}

// Line entries are packed two-vertices-per-DebugLineEntry; the attribute layout below
// matches GameTechRenderer's so we can upload Debug::GetDebugLines() verbatim.
void DistributedClientRenderer::SetLineBufferSizes(size_t entryCount) {
	if (entryCount <= mLineCount) {
		return;
	}
	mLineCount = entryCount;

	glBindBuffer(GL_ARRAY_BUFFER, mLineVBO);
	glBufferData(GL_ARRAY_BUFFER, mLineCount * sizeof(Debug::DebugLineEntry), nullptr, GL_DYNAMIC_DRAW);

	glBindVertexArray(mLineVAO);
	const int realStride = sizeof(Debug::DebugLineEntry) / 2;

	glVertexAttribFormat(0, 3, GL_FLOAT, false, offsetof(Debug::DebugLineEntry, start));
	glVertexAttribBinding(0, 0);
	glBindVertexBuffer(0, mLineVBO, 0, realStride);

	glVertexAttribFormat(1, 4, GL_FLOAT, false, offsetof(Debug::DebugLineEntry, colourA));
	glVertexAttribBinding(1, 0);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);
}

void DistributedClientRenderer::SetTextBufferSizes(size_t vertCount) {
	if (vertCount <= mTextCount) {
		return;
	}
	mTextCount = vertCount;

	glBindBuffer(GL_ARRAY_BUFFER, mTextVertVBO);
	glBufferData(GL_ARRAY_BUFFER, mTextCount * sizeof(Vector3), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, mTextColourVBO);
	glBufferData(GL_ARRAY_BUFFER, mTextCount * sizeof(Vector4), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, mTextTexVBO);
	glBufferData(GL_ARRAY_BUFFER, mTextCount * sizeof(Vector2), nullptr, GL_DYNAMIC_DRAW);

	glBindVertexArray(mTextVAO);

	glVertexAttribFormat(0, 3, GL_FLOAT, false, 0);
	glVertexAttribBinding(0, 0);
	glBindVertexBuffer(0, mTextVertVBO, 0, sizeof(Vector3));

	glVertexAttribFormat(1, 4, GL_FLOAT, false, 0);
	glVertexAttribBinding(1, 1);
	glBindVertexBuffer(1, mTextColourVBO, 0, sizeof(Vector4));

	glVertexAttribFormat(2, 2, GL_FLOAT, false, 0);
	glVertexAttribBinding(2, 2);
	glBindVertexBuffer(2, mTextTexVBO, 0, sizeof(Vector2));

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);

	glBindVertexArray(0);
}
#endif
