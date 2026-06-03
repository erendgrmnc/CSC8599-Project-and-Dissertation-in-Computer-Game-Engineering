#include "ProfilerRenderer.h"
#include "Profiler.h"
#include "Win32Window.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_win32.h"

#include <cstdio>
#include <string>

namespace {
	const ImVec4 kAccent = ImVec4(0.40f, 0.66f, 1.00f, 1.00f);
	const ImVec4 kValue  = ImVec4(0.86f, 0.91f, 0.98f, 1.00f);
	const ImVec4 kMuted  = ImVec4(0.58f, 0.62f, 0.70f, 1.00f);

	std::string Ms(float v) {
		char b[32];
		std::snprintf(b, sizeof(b), "%.2f ms", v);
		return b;
	}

	std::string F0(float v) {
		char b[32];
		std::snprintf(b, sizeof(b), "%.0f", v);
		return b;
	}

	std::string YesNo(bool v) { return v ? "Yes" : "No"; }

	// A section is open by default so the metrics are visible immediately.
	bool Section(const char* label) {
		return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
	}

	void BeginMetrics(const char* id) {
		ImGui::BeginTable(id, 2,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
			ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp);
		ImGui::TableSetupColumn("metric", ImGuiTableColumnFlags_WidthStretch, 0.62f);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.38f);
	}

	void Row(const char* label, const std::string& value) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::PushStyleColor(ImGuiCol_Text, kValue);
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
	}
}

ProfilerRenderer::ProfilerRenderer(Window& w, ProfilerType type, bool isImguiInited) : OGLRenderer(w), profilerType(type) {
	if (!isImguiInited) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		(void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.FontGlobalScale = 1.25f; // default font is tiny; scale it up for readability

		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowPadding = ImVec2(16, 16);
		style.FramePadding = ImVec2(8, 5);
		style.CellPadding = ImVec2(8, 6);
		style.ItemSpacing = ImVec2(8, 8);
		style.WindowRounding = 0.0f;
		style.FrameRounding = 4.0f;
		style.ChildRounding = 6.0f;

		ImVec4* c = style.Colors;
		c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
		c[ImGuiCol_Header] = ImVec4(0.15f, 0.30f, 0.50f, 0.75f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.65f, 0.85f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.44f, 0.70f, 1.00f);
		c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.035f);
		c[ImGuiCol_TableBorderLight] = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
		c[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);

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
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);

	glClearColor(0.07f, 0.08f, 0.10f, 1.0f); // match the imgui window background
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	RenderProfilerUI();
}

void ProfilerRenderer::EndFrame() {
	OGLRenderer::EndFrame();
}

void ProfilerRenderer::RenderProfilerUI() {
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Fill the OS window with a single panel instead of a small floating window.
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);

	ImGui::Begin("Profiler", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	const char* title = "Profiler";
	switch (profilerType) {
	case ProfilerType::DistributedClient: title = "Distributed Client"; break;
	case ProfilerType::DistributedPhysicsServer: title = "Physics Server"; break;
	case ProfilerType::DistributedPhysicsServerManager: title = "Distributed Manager"; break;
	case ProfilerType::DistributedPhysicsMidware: title = "Physics Midware"; break;
	}

	ImGui::SetWindowFontScale(1.4f);
	ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
	ImGui::TextUnformatted(title);
	ImGui::PopStyleColor();
	ImGui::SetWindowFontScale(1.0f);

	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	ImGui::TextUnformatted("Distributed Physics System");
	ImGui::PopStyleColor();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	switch (profilerType) {
	case ProfilerType::DistributedClient: RenderDistributedClientAttributes(); break;
	case ProfilerType::DistributedPhysicsServer: RenderDistributedGameServerAttributes(); break;
	case ProfilerType::DistributedPhysicsServerManager: RenderDistributedGameServerManagerAttributes(); break;
	case ProfilerType::DistributedPhysicsMidware: RenderPhysicsServerMidwareAttributes(); break;
	}

	ImGui::Spacing();
	RenderMemoryUsage();

	ImGui::End();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ProfilerRenderer::RenderMemoryUsage() {
	if (Section("Memory Usage")) {
		BeginMetrics("MemoryTable");
		Row("Virtual Memory (program)", Profiler::GetVirtualMemoryUsageByProgram());
		Row("Virtual Memory (system)", Profiler::GetVirtualMemoryUsage());
		Row("Total Virtual Memory", Profiler::GetTotalVirtualMemory());
		Row("Physical Memory (program)", Profiler::GetPhysicalMemoryUsageByProgram());
		Row("Physical Memory (system)", Profiler::GetPhysicalMemoryUsage());
		Row("Total Physical Memory", Profiler::GetTotalPhysicalMemory());
		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderDistributedGameServerAttributes() {
	if (Section("Physics Server")) {
		BeginMetrics("ServerTable");
		Row("Total Created Objects", std::to_string(Profiler::GetTotalObjectsInServer()));
		Row("Objects In Server", std::to_string(Profiler::GetObjectsOnBorders()));
		Row("Physics Time", Ms(Profiler::GetPhysicsTime()));
		Row("Physics Prediction Time", Ms(Profiler::GetPhysicsPredictionTime()));
		Row("World Time", Ms(Profiler::GetWorldTime()));
		Row("Full Snapshot Time", Ms(Profiler::GetLastFullSnapshotTime()));
		Row("Delta Snapshot Time", Ms(Profiler::GetLastDeltaSnapshotTime()));
		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderDistributedClientAttributes() {
	if (Section("Distributed Client")) {
		BeginMetrics("ClientTable");
		Row("FPS", F0(Profiler::GetFramesPerSecond()));
		Row("Network Time", Ms(Profiler::GetNetworkTime()));
		Row("Time Per Update", Ms(Profiler::GetTimePassedPerUpdate()));
		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderDistributedGameServerManagerAttributes() {
	if (Section("Distributed Manager")) {
		BeginMetrics("ManagerTable");
		Row("Connected Midwares", std::to_string(Profiler::GetConnectedPhysicsServerMiddlewares()));
		Row("Connected Game Clients", std::to_string(Profiler::GetConnectedGameClients()));
		Row("Started Game Instances", std::to_string(Profiler::GetStartedGameInstances()));
		ImGui::EndTable();
	}
}

void ProfilerRenderer::RenderPhysicsServerMidwareAttributes() {
	if (Section("Physics Midware")) {
		BeginMetrics("MidwareTable");
		Row("Connected To Manager", YesNo(Profiler::GetIsConnectedToGameManager()));
		ImGui::EndTable();
	}
}
