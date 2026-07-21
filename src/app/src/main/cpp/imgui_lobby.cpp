#include "imgui_lobby.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "log.h"

#include <cmath>
#include <algorithm>

static imgui_lobby * gImguiLobby = nullptr;

imgui_lobby::imgui_lobby()
{
}

imgui_lobby::~imgui_lobby()
{
	shutdown();
}

void imgui_lobby::init()
{
	if (initialized)
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO & io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	// Dark theme with slightly larger scale for VR readability
	ImGui::StyleColorsDark();
	ImGuiStyle & style = ImGui::GetStyle();
	style.WindowRounding = 8.0f;
	style.FrameRounding = 4.0f;
	style.GrabRounding = 4.0f;
	style.WindowPadding = ImVec2(16, 16);
	style.FramePadding = ImVec2(12, 8);
	style.ItemSpacing = ImVec2(10, 8);
	style.ScrollbarSize = 20.0f;
	style.ScaleAllSizes(1.3f);

	// Use the default embedded font at a larger size
	ImFontConfig font_cfg;
	font_cfg.SizePixels = 24.0f;
	io.Fonts->AddFontDefault(&font_cfg);

	ImGui_ImplOpenGL3_Init("#version 300 es");

	glGenFramebuffers(1, &fbo);

	initialized = true;
	LOGI("imgui_lobby initialized");
}

void imgui_lobby::shutdown()
{
	if (!initialized)
		return;

	if (fbo)
	{
		glDeleteFramebuffers(1, &fbo);
		fbo = 0;
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext();

	initialized = false;
}

void imgui_lobby::set_servers(const std::vector<server_entry> & s)
{
	servers = s;
}

void imgui_lobby::draw_server_list()
{
	ImGui::BeginChild("server_list", ImVec2(0, 0), true);

	if (servers.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.5f));
		const char * msg = "Start a WiVRn server on your\nlocal network";
		ImVec2 sz = ImGui::CalcTextSize(msg);
		ImVec2 pos = ImGui::GetCursorPos();
		ImVec2 region = ImGui::GetContentRegionAvail();
		ImGui::SetCursorPos(ImVec2(pos.x + (region.x - sz.x) * 0.5f,
		                           pos.y + (region.y - sz.y) * 0.5f));
		ImGui::TextUnformatted(msg);
		ImGui::PopStyleColor();
	}
	else
	{
		for (const auto & srv : servers)
		{
			ImGui::PushID(srv.hostname.c_str());

			float avail = ImGui::GetContentRegionAvail().x;
			float btn_w = 160.0f;
			float item_h = 80.0f;

			// Server name
			ImGui::Text("%s", srv.name.c_str());
			if (!srv.manual)
			{
				ImGui::SameLine(0, 10);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.5f, 1));
				ImGui::Text("  (auto)");
				ImGui::PopStyleColor();
			}

			ImGui::TextDisabled("%s:%d", srv.hostname.c_str(), srv.port);

			// Autoconnect checkbox for auto-discovered servers
			if (!srv.manual)
			{
				ImGui::Checkbox("Autoconnect", const_cast<bool *>(&srv.autoconnect));
			}

			// Connect button on the right
			ImGui::SameLine(avail - btn_w);
			bool can_connect = (srv.visible && srv.compatible) || srv.manual;
			ImGui::BeginDisabled(!can_connect);
			if (can_connect)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.3f, 0.4f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.8f, 0.3f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 1.0f, 0.2f, 1.0f));
			}
			if (ImGui::Button("Connect", ImVec2(btn_w, 40)))
			{
				if (on_connect)
					on_connect(srv);
			}
			if (can_connect)
				ImGui::PopStyleColor(3);
			ImGui::EndDisabled();

			ImGui::Separator();
			ImGui::PopID();
		}
	}

	ImGui::EndChild();
}

void imgui_lobby::draw_settings()
{
	ImGui::BeginChild("settings", ImVec2(0, 0), true);

	ImGui::Text("Video");
	ImGui::Checkbox("Passthrough", &setting_passthrough);
	ImGui::SliderInt("Bitrate", &setting_bitrate, 10, 200, "%d Mbps");
	ImGui::Separator();

	ImGui::Text("Audio");
	ImGui::Checkbox("Microphone", &setting_microphone);
	ImGui::Separator();

	ImGui::Text("Network");
	ImGui::Checkbox("TCP only", &setting_tcp_only);
	ImGui::Separator();

	ImGui::Text("Tracking");
	ImGui::SliderFloat("IPD (mm)", &setting_ipd, 50.0f, 80.0f, "%.1f");

	ImGui::EndChild();
}

void imgui_lobby::draw_about()
{
	ImGui::BeginChild("about", ImVec2(0, 0), true);
	ImGui::Text("WiVRn for Pico Neo 2");
	ImGui::TextDisabled("3D ImGui UI POC");
	ImGui::Spacing();
	ImGui::Text("Streaming client for PC VR");
	ImGui::Text("Powered by ImGui + OpenGL ES 3.0");
	ImGui::Spacing();
	ImGui::Text("https://github.com/wivrn/wivrn");
	ImGui::EndChild();
}

void imgui_lobby::render(GLuint target_texture, int width, int height,
                         float hit_u, float hit_v, bool has_hit,
                         bool trigger_down, bool trigger_held,
                         float thumbstick_y)
{
	if (!initialized)
		init();

	// Recreate font texture if needed (ImGui marks it dirty after init)
	ImGuiIO & io = ImGui::GetIO();
	if (!io.Fonts->IsBuilt())
		ImGui_ImplOpenGL3_CreateDeviceObjects();

	// Set display size
	io.DisplaySize = ImVec2((float)width, (float)height);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

	// Map ray-cast hit to ImGui mouse position
	// hit_u/hit_v are in -1..1 panel space
	// ImGui mouse is in 0..width, 0..height
	if (has_hit)
	{
		float mx = ((hit_u + 1.0f) * 0.5f) * (float)width;
		float my = (1.0f - (hit_v + 1.0f) * 0.5f) * (float)height;
		io.AddMousePosEvent(mx, my);
		io.AddMouseButtonEvent(0, trigger_held);
	}
	else
	{
		io.AddMousePosEvent(-1, -1);
		io.AddMouseButtonEvent(0, false);
	}

	// Thumbstick for scrolling
	io.AddMouseWheelEvent(0.0f, -thumbstick_y * 0.5f);

	// Bind FBO with the target texture
	GLint prev_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, target_texture, 0);

	GLint prev_viewport[4];
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	glViewport(0, 0, width, height);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);

	// Start ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	// Main lobby window: fills the entire panel
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##lobby", nullptr,
	             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
	             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
	             ImGuiWindowFlags_NoBringToFrontOnFocus);

	// Tab bar
	if (ImGui::BeginTabBar("##tabs"))
	{
		if (ImGui::BeginTabItem("Servers"))
		{
			current_tab = tab::server_list;
			draw_server_list();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Settings"))
		{
			current_tab = tab::settings;
			draw_settings();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("About"))
		{
			current_tab = tab::about;
			draw_about();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();

	// Render
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	// Restore state
	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glDisable(GL_BLEND);

	tex_w = width;
	tex_h = height;
}
