#include "ProfilerRenderer.h"
#include "Profiler.h"
#include "Win32Window.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_win32.h"

ProfilerRenderer::ProfilerRenderer(Window& w, ProfilerType type, bool isImguiInited) : OGLRenderer(w), profilerType(type) {
	// Setup Dear ImGui context

	if (!isImguiInited) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		(void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
		// Setup Platform/Renderer backends
		mWindowWidth = 400;
		mWindowHeight = 700;

		ImGui_ImplWin32_InitForOpenGL(Win32Code::Win32Window::windowHandle);
		ImGui_ImplOpenGL3_Init();
	}
}

ProfilerRenderer::~ProfilerRenderer() {
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}


void ProfilerRenderer::BeginFrame() {
	OGLRenderer::BeginFrame();
}

void ProfilerRenderer::RenderFrame() {
	glEnable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 0); // Set clear color to transparent

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Render 3D objects here

	glDisable(GL_DEPTH_TEST); // Disable depth testing for UI
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	RenderProfilerUI();

	glEnable(GL_DEPTH_TEST); // Re-enable depth testing for 3D objects
}

void ProfilerRenderer::EndFrame() {
	OGLRenderer::EndFrame();
}

void ProfilerRenderer::RenderProfilerUI() {
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::Begin("Profiler", NULL,
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize
	);

	ImGui::Spacing();
	switch (profilerType) {
	case ProfilerType::DistributedClient: {
		RenderDistributedClientAttributes();
		break;
	}
	case ProfilerType::DistributedPhysicsServer: {
		RenderDistributedGameServerAttributes();
		break;
	}
	case ProfilerType::DistributedPhysicsServerManager: {
		break;
	}
	case ProfilerType::DistributedPhysicsMidware: {
		break;
	}
	}

	RenderMemoryUsage();

	ImGui::End();

	ImGui::Render();
	ImGui::UpdatePlatformWindows();
	ImGui::RenderPlatformWindowsDefault();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ProfilerRenderer::RenderMemoryUsage() {
	if (ImGui::CollapsingHeader("Memory Usage"))
	{
		ImGui::BeginTable("Memory Usage Table", 2);

		ImGui::TableNextColumn();
		ImGui::Text("Virtual Memory By Program");
		ImGui::TableNextColumn();
		const std::string& memStr = Profiler::GetVirtualMemoryUsageByProgram();

		ImGui::Text(memStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Virtual Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetVirtualMemoryUsage().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Total Virtual Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetTotalVirtualMemory().c_str());

		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();

		ImGui::Text("Physical Memory By Program");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetPhysicalMemoryUsageByProgram().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Physical Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetPhysicalMemoryUsage().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Total Physical Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetTotalPhysicalMemory().c_str());

		ImGui::EndTable();
	}
}



void ProfilerRenderer::RenderDistributedGameServerAttributes() {

	if (ImGui::CollapsingHeader("Distributed Physics Server Attributes"))
	{
		ImGui::BeginTable("Game Object Information", 2);

		ImGui::TableNextColumn();
		ImGui::Text("Total Created Game Objects");
		ImGui::TableNextColumn();
		const std::string objCountStr = std::to_string(Profiler::GetTotalObjectsInServer());

		ImGui::Text(objCountStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Objects In Server");
		ImGui::TableNextColumn();
		std::string objInServerStr = std::to_string(Profiler::GetObjectsOnBorders());
		ImGui::Text(objInServerStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Physics Time");
		ImGui::TableNextColumn();
		std::string physicsTime = std::to_string(Profiler::GetPhysicsTime());
		ImGui::Text(physicsTime.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Physics Prediction Time");
		ImGui::TableNextColumn();
		std::string physicsPredictionTime = std::to_string(Profiler::GetPhysicsPredictionTime());
		ImGui::Text(physicsPredictionTime.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("World Time");
		ImGui::TableNextColumn();
		std::string worldTimeStr = std::to_string(Profiler::GetWorldTime());
		ImGui::Text(worldTimeStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Full Snapshot Time");
		ImGui::TableNextColumn();
		std::string fullPacketTimeStr = std::to_string(Profiler::GetLastFullSnapshotTime());
		ImGui::Text(fullPacketTimeStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Delta Snapshot Time");
		ImGui::TableNextColumn();
		std::string deltaPacketTimeStr = std::to_string(Profiler::GetLastDeltaSnapshotTime());
		ImGui::Text(deltaPacketTimeStr.c_str());

		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();


		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderDistributedClientAttributes() {

	if (ImGui::CollapsingHeader("Distributed Client Attributes"))
	{
		ImGui::BeginTable("Client Attributes", 2);

		ImGui::TableNextColumn();
		ImGui::Text("FPS");
		ImGui::TableNextColumn();
		const std::string fpsStr = std::to_string(Profiler::GetFramesPerSecond());

		ImGui::Text(fpsStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Network Time");
		ImGui::TableNextColumn();
		std::string objInServerStr = std::to_string(Profiler::GetObjectsOnBorders());
		ImGui::Text(objInServerStr.c_str());


		ImGui::Text("Time passed per update");
		ImGui::TableNextColumn();
		std::string timePassedPerUpdateStr = std::to_string(Profiler::GetTimePassedPerUpdate());
		ImGui::Text(timePassedPerUpdateStr.c_str());


		ImGui::TableNextColumn();

		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();


		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderPhysicsServerAttributes() {
	if (ImGui::CollapsingHeader("Physics Server Midware Attributes"))
	{
		ImGui::BeginTable("Connection Attributes", 2);

		ImGui::TableNextColumn();
		ImGui::Text("Is connected to manager");
		ImGui::TableNextColumn();
		const std::string isConnectedToManagerStr = std::to_string(0);

		ImGui::Text(isConnectedToManagerStr.c_str());

		ImGui::TableNextColumn();
		ImGui::Text("Started physics server instance");
		ImGui::TableNextColumn();
		const std::string physicsServerInstanceCtr = std::to_string(0);

		ImGui::Text(physicsServerInstanceCtr.c_str());


		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();

		ImGui::EndTable();
	}
}
