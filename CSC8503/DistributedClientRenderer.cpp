#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "DistributedClientRenderer.h"

#include "OGLShader.h"
#include "OGLMesh.h"
#include "MshLoader.h"
#include "GameWorld.h"
#include "GameObject.h"
#include "RenderObject.h"
#include "Camera.h"

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

	std::cout << "Distributed client forward renderer ready.\n";
}

DistributedClientRenderer::~DistributedClientRenderer() {
	delete mShader;
	delete mCubeMesh;
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
}
#endif
