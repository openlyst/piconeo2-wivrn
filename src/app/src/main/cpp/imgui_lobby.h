#pragma once

#include <GLES3/gl3.h>
#include <string>
#include <vector>
#include <functional>

// Minimal ImGui-based 3D lobby UI (POC).
// Renders directly to a GL texture via an FBO, replacing the Java Canvas
// upload path. Controller ray-cast hits are mapped to ImGui mouse input.
class imgui_lobby
{
public:
	struct server_entry
	{
		std::string name;
		std::string hostname;
		int port = 0;
		bool tcp_only = false;
		bool autoconnect = false;
		bool manual = false;
		bool visible = true;
		bool compatible = true;
	};

	// Callback when the user clicks Connect on a server.
	std::function<void(const server_entry &)> on_connect;

	// Callback when the user clicks "Add server".
	std::function<void()> on_add_server;

	// Callback to quit the app.
	std::function<void()> on_quit;

	imgui_lobby();
	~imgui_lobby();

	void init();
	void shutdown();

	// Set the list of discovered/known servers.
	void set_servers(const std::vector<server_entry> & servers);

	// Render the ImGui UI to the given texture.
	// hit_u/hit_v: ray-cast hit in -1..1 panel space, or NaN if no hit.
	// trigger_down: trigger just pressed this frame.
	// trigger_held: trigger held down.
	void render(GLuint target_texture, int width, int height,
	            float hit_u, float hit_v, bool has_hit,
	            bool trigger_down, bool trigger_held,
	            float thumbstick_y);

	bool is_initialized() const { return initialized; }

private:
	bool initialized = false;
	GLuint fbo = 0;
	int tex_w = 0;
	int tex_h = 0;

	std::vector<server_entry> servers;

	enum class tab { server_list, settings, about };
	tab current_tab = tab::server_list;

	// Settings state (POC - will be wired to real config later)
	bool setting_passthrough = true;
	bool setting_microphone = false;
	bool setting_tcp_only = false;
	int  setting_bitrate = 50;
	float setting_ipd = 64.0f;

	void draw_server_list();
	void draw_settings();
	void draw_about();
};
