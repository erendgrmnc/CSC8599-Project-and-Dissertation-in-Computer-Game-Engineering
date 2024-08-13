#pragma once
#include "OGLRenderer.h"


namespace NCL {
	enum ProfilerType{
		DistributedPhysicsServer,
		DistributedPhysicsServerManager,
		DistributedClient,
	};

	class ProfilerRenderer : public OGLRenderer {
	public:
		ProfilerRenderer(Window& w, ProfilerType type, bool isImguiInited = false);
		~ProfilerRenderer();

	protected:
		ProfilerType profilerType;
		int mWindowWidth;
		int mWindowHeight;

		void BeginFrame() override;
		void RenderFrame() override;
		void EndFrame() override;
		void RenderProfilerUI();
		void RenderMemoryUsage();
		void RenderDistributedGameServerAttributes();
		void RenderDistributedClientAttributes();
	};

}