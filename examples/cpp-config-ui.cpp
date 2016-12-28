#define _USE_MATH_DEFINES

#include <librealsense/rs.hpp>
#include "example.hpp"

#define GLFW_INCLUDE_GLU
#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_impl_glfw.h"

#include <cstdarg>
#include <thread>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>


#pragma comment(lib, "opengl32.lib")



class option_model
{
public:
    rs_option opt;
    rs::option_range range;
    rs::device endpoint;
    bool* invalidate_flag;
    bool supported = false;
    float value = 0.0f;
    std::string label = "";
    std::string id = "";

    void draw(std::string& error_message)
    {
        if (supported)
        {
            if (is_checkbox())
            {
                auto bool_value = value > 0.0f;
                if (ImGui::Checkbox(label.c_str(), &bool_value))
                {
                    value = bool_value ? 1.0f : 0.0f;
                    try
                    {
                        endpoint.set_option(opt, value);
                        *invalidate_flag = true;
                    }
                    catch (const rs::error& e)
                    {
                        error_message = error_to_string(e);
                    }
                }
            }
            else
            {
                std::string txt = to_string() << label << ":";
                ImGui::Text(txt.c_str());
                ImGui::PushItemWidth(-1);

                try
                {
                    if (is_enum())
                    {
                        std::vector<const char*> labels;
                        auto selected = 0, counter = 0;
                        for (auto i = range.min; i <= range.max; i += range.step, counter++)
                        {
                            if (abs(i - value) < 0.001f) selected = counter;
                            labels.push_back(endpoint.get_option_value_description(opt, i));
                        }
                        if (ImGui::Combo(id.c_str(), &selected, labels.data(),
                            static_cast<int>(labels.size())))
                        {
                            value = range.min + range.step * selected;
                            endpoint.set_option(opt, value);
                            *invalidate_flag = true;
                        }
                    }
                    else if (is_all_integers())
                    {
                        auto int_value = static_cast<int>(value);
                        if (ImGui::SliderInt(id.c_str(), &int_value,
                            static_cast<int>(range.min),
                            static_cast<int>(range.max)))
                        {
                            // TODO: Round to step?
                            value = static_cast<float>(int_value);
                            endpoint.set_option(opt, value);
                            *invalidate_flag = true;
                        }
                    }
                    else
                    {
                        if (ImGui::SliderFloat(id.c_str(), &value,
                            range.min, range.max))
                        {
                            endpoint.set_option(opt, value);
                        }
                    }
                }
                catch (const rs::error& e)
                {
                    error_message = error_to_string(e);
                }
                ImGui::PopItemWidth();
            }

            auto desc = endpoint.get_option_description(opt);
            if (ImGui::IsItemHovered() && desc)
            {
                ImGui::SetTooltip(desc);
            }
        }
    }

    void update(std::string& error_message)
    {
        try
        {
            if (endpoint.supports(opt))
                value = endpoint.get_option(opt);
        }
        catch (const rs::error& e)
        {
            error_message = error_to_string(e);
        }
    }
private:
    bool is_all_integers() const
    {
        return is_integer(range.min) && is_integer(range.max) &&
            is_integer(range.def) && is_integer(range.step);
    }

    bool is_enum() const
    {
        for (auto i = range.min; i <= range.max; i += range.step)
        {
            if (endpoint.get_option_value_description(opt, i) == nullptr)
                return false;
        }
        return true;
    }

    bool is_checkbox() const
    {
        return range.max == 1.0f &&
            range.min == 0.0f &&
            range.step == 1.0f;
    }
};

template<class T>
void push_back_if_not_exists(std::vector<T>& vec, T value)
{
    auto it = std::find(vec.begin(), vec.end(), value);
    if (it == vec.end()) vec.push_back(value);
}

std::vector<const char*> get_string_pointers(const std::vector<std::string>& vec)
{
    std::vector<const char*> res;
    for (auto&& s : vec) res.push_back(s.c_str());
    return res;
}

class subdevice_model
{
public:
    subdevice_model(rs::device dev, std::string& error_message)
        : dev(dev), streaming(false), queues(RS_STREAM_COUNT)
    {
        for (auto& elem : queues)
        {
            elem = std::unique_ptr<rs::frame_queue>(new rs::frame_queue(5));
        }

        for (auto i = 0; i < RS_OPTION_COUNT; i++)
        {
            option_model metadata;
            auto opt = static_cast<rs_option>(i);

            std::stringstream ss;
            ss << dev.get_camera_info(RS_CAMERA_INFO_DEVICE_NAME)
               << "/" << dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME)
               << "/" << rs_option_to_string(opt);
            metadata.id = ss.str();
            metadata.opt = opt;
            metadata.endpoint = dev;
            metadata.label = rs_option_to_string(opt);
            metadata.invalidate_flag = &options_invalidated;

            metadata.supported = dev.supports(opt);
            if (metadata.supported)
            {
                try
                {
                    metadata.range = dev.get_option_range(opt);
                    metadata.value = dev.get_option(opt);
                }
                catch (const rs::error& e)
                {
                    metadata.range = { 0, 1, 0, 0 };
                    metadata.value = 0;
                    error_message = error_to_string(e);
                }
            }
            options_metadata[opt] = metadata;
        }

        try
        {
            auto uvc_profiles = dev.get_stream_modes();
            for (auto&& profile : uvc_profiles)
            {
                std::stringstream res;
                res << profile.width << " x " << profile.height;
                push_back_if_not_exists(res_values, std::pair<int, int>(profile.width, profile.height));
                push_back_if_not_exists(resolutions, res.str());
                std::stringstream fps;
                fps << profile.fps;
                push_back_if_not_exists(fps_values, profile.fps);
                push_back_if_not_exists(fpses, fps.str());
                std::string format = rs_format_to_string(profile.format);

                push_back_if_not_exists(formats[profile.stream], format);
                push_back_if_not_exists(format_values[profile.stream], profile.format);

                auto any_stream_enabled = false;
                for (auto it : stream_enabled)
                {
                    if (it.second)
                    {
                        any_stream_enabled = true;
                        break;
                    }
                }
                if (!any_stream_enabled) stream_enabled[profile.stream] = true;

                profiles.push_back(profile);
            }

            // set default selections
            int selection_index;

            get_default_selection_index(res_values, std::pair<int,int>(640,480), &selection_index);
            selected_res_id = selection_index;

            get_default_selection_index(fps_values, 30, &selection_index);
            selected_fps_id = selection_index;

            for (auto format_array : format_values)
            {
                for (auto format : { rs_format::RS_FORMAT_RGB8, rs_format::RS_FORMAT_Z16, rs_format::RS_FORMAT_Y8 } )
                {
                    if (get_default_selection_index(format_array.second, format, &selection_index))
                    {
                        selected_format_id[format_array.first] = selection_index;
                        break;
                    }
                }
            }
        }
        catch (const rs::error& e)
        {
            error_message = error_to_string(e);
        }
    }

    bool is_selected_combination_supported()
    {
        std::vector<rs::stream_profile> results;

        for (auto i = 0; i < RS_STREAM_COUNT; i++)
        {
            auto stream = static_cast<rs_stream>(i);
            if (stream_enabled[stream])
            {
                auto width = res_values[selected_res_id].first;
                auto height = res_values[selected_res_id].second;
                auto fps = fps_values[selected_fps_id];
                auto format = format_values[stream][selected_format_id[stream]];

                for (auto&& p : profiles)
                {
                    if (p.width == width && p.height == height && p.fps == fps && p.format == format)
                        results.push_back(p);
                }
            }
        }
        return results.size() > 0;
    }

    std::vector<rs::stream_profile> get_selected_profiles()
    {
        std::vector<rs::stream_profile> results;

        std::stringstream error_message;
        error_message << "The profile ";

        for (auto i = 0; i < RS_STREAM_COUNT; i++)
        {
            auto stream = static_cast<rs_stream>(i);
            if (stream_enabled[stream])
            {
                auto width = res_values[selected_res_id].first;
                auto height = res_values[selected_res_id].second;
                auto fps = fps_values[selected_fps_id];
                auto format = format_values[stream][selected_format_id[stream]];

                error_message << "\n{" << rs_stream_to_string(stream) << ","
                              << width << "x" << height << " at " << fps << "Hz, "
                              << rs_format_to_string(format) << "} ";

                for (auto&& p : profiles)
                {
                    if (p.width == width &&
                        p.height == height &&
                        p.fps == fps &&
                        p.format == format &&
                        p.stream == stream)
                        results.push_back(p);
                }
            }
        }
        if (results.size() == 0)
        {
            error_message << " is unsupported!";
            throw std::runtime_error(error_message.str());
        }
        return results;
    }

    void stop()
    {
        streaming = false;

        for (auto& elem : queues)
            elem->flush();

        dev.stop();
        dev.close();
    }

    void play(const std::vector<rs::stream_profile>& profiles)
    {
        dev.open(profiles);
        try {
            dev.start([&](rs::frame f){
                auto stream_type = f.get_stream_type();
                queues[(int)stream_type]->enqueue(std::move(f));
            });
        }
        catch (...)
        {
            dev.close();
            throw;
        }

        streaming = true;
    }

    void update(std::string& error_message)
    {
        if (options_invalidated)
        {
            next_option = 0;
            options_invalidated = false;
        }
        if (next_option < RS_OPTION_COUNT)
        {
            options_metadata[static_cast<rs_option>(next_option)].update(error_message);
            next_option++;
        }
    }

    template<typename T>
    bool get_default_selection_index(const std::vector<T>& values, const T & def, int* index /*std::function<int(const std::vector<T>&,T,int*)> compare = nullptr*/)
    {
        auto max_default = values.begin();
        for (auto it = values.begin(); it != values.end(); it++)
        {

            if (*it == def)
            {
                *index = (int)(it - values.begin());
                return true;
            }
            if (*max_default < *it)
            {
                max_default = it;
            }
        }
        *index = (int)(max_default - values.begin());
        return false;
    }

    rs::device dev;

    std::map<rs_option, option_model> options_metadata;
    std::vector<std::string> resolutions;
    std::vector<std::string> fpses;
    std::map<rs_stream, std::vector<std::string>> formats;
    std::map<rs_stream, bool> stream_enabled;

    int selected_res_id = 0;
    int selected_fps_id = 0;
    std::map<rs_stream, int> selected_format_id;

    std::vector<std::pair<int, int>> res_values;
    std::vector<int> fps_values;
    std::map<rs_stream, std::vector<rs_format>> format_values;

    std::vector<rs::stream_profile> profiles;

    std::vector<std::unique_ptr<rs::frame_queue>> queues;
    bool options_invalidated = false;
    int next_option = RS_OPTION_COUNT;
    bool streaming;
};

typedef std::map<rs_stream, rect> streams_layout;

class device_model
{
public:
    explicit device_model(rs::device& dev, std::string& error_message)
    {
        for (auto&& sub : dev.get_adjacent_devices())
        {
            auto model = std::make_shared<subdevice_model>(sub, error_message);
            subdevices.push_back(model);
        }
    }


    bool is_stream_visible(rs_stream s)
    {
        using namespace std::chrono;
        auto now = high_resolution_clock::now();
        auto diff = now - steam_last_frame[s];
        auto ms = duration_cast<milliseconds>(diff).count();
        return ms <= _frame_timeout + _min_timeout;
    }

    float get_stream_alpha(rs_stream s)
    {
        using namespace std::chrono;
        auto now = high_resolution_clock::now();
        auto diff = now - steam_last_frame[s];
        auto ms = duration_cast<milliseconds>(diff).count();
        auto t = smoothstep(static_cast<float>(ms),
                            _min_timeout, _min_timeout + _frame_timeout);
        return 1.0f - t;
    }

    std::map<rs_stream, rect> calc_layout(int x0, int y0, int width, int height)
    {
        std::vector<rs_stream> active_streams;
        for (auto i = 0; i < RS_STREAM_COUNT; i++)
        {
            auto stream = static_cast<rs_stream>(i);
            if (is_stream_visible(stream))
            {
                active_streams.push_back(stream);
            }
        }

        if (fullscreen)
        {
            auto it = std::find(begin(active_streams), end(active_streams), selected_stream);
            if (it == end(active_streams)) fullscreen = false;
        }

        std::map<rs_stream, rect> results;

        if (fullscreen)
        {
            results[selected_stream] = { x0, y0, width, height };
        }
        else
        {
            auto factor = ceil(sqrt(active_streams.size()));
            auto complement = ceil(active_streams.size() / factor);

            auto cell_width = static_cast<float>(width / factor);
            auto cell_height = static_cast<float>(height / complement);

            auto i = 0;
            for (auto x = 0; x < factor; x++)
            {
                for (auto y = 0; y < complement; y++)
                {
                    if (i == active_streams.size()) break;

                    rect r = { x0 + x * cell_width, y0 + y * cell_height,
                        cell_width, cell_height };
                    results[active_streams[i]] = r;
                    i++;
                }
            }
        }

        return get_interpolated_layout(results);
    }

    std::vector<std::shared_ptr<subdevice_model>> subdevices;
    std::map<rs_stream, texture_buffer> stream_buffers;
    std::map<rs_stream, float2> stream_size;
    std::map<rs_stream, rs_format> stream_format;
    std::map<rs_stream, std::chrono::high_resolution_clock::time_point> steam_last_frame;

    bool fullscreen = false;
    rs_stream selected_stream = RS_STREAM_ANY;

private:
    std::map<rs_stream, rect> get_interpolated_layout(const std::map<rs_stream, rect>& l)
    {
        using namespace std::chrono;
        auto now = high_resolution_clock::now();
        if (l != _layout) // detect layout change
        {
            _transition_start_time = now;
            _old_layout = _layout;
            _layout = l;
        }

        //if (_old_layout.size() == 0 && l.size() == 1) return l;

        auto diff = now - _transition_start_time;
        auto ms = duration_cast<milliseconds>(diff).count();
        auto t = smoothstep(static_cast<float>(ms), 0, 100);

        std::map<rs_stream, rect> results;
        for (auto&& kvp : l)
        {
            auto stream = kvp.first;
            if (_old_layout.find(stream) == _old_layout.end())
            {
                _old_layout[stream] = _layout[stream].center();
            }
            results[stream] = _old_layout[stream].lerp(t, _layout[stream]);
        }

        return results;
    }

    float _frame_timeout = 700.0f;
    float _min_timeout = 90.0f;

    streams_layout _layout;
    streams_layout _old_layout;
    std::chrono::high_resolution_clock::time_point _transition_start_time;
};

bool no_device_popup(GLFWwindow* window, const ImVec4& clear_color)
{
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        int w, h;
        glfwGetWindowSize(window, &w, &h);

        ImGui_ImplGlfw_NewFrame();

        // Rendering 
        glViewport(0, 0,
            static_cast<int>(ImGui::GetIO().DisplaySize.x),
            static_cast<int>(ImGui::GetIO().DisplaySize.y));
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ static_cast<float>(w), static_cast<float>(h) });
        ImGui::Begin("", nullptr, flags);

        ImGui::OpenPopup("config-ui");
        if (ImGui::BeginPopupModal("config-ui", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            // 
            ImGui::Text("No device detected. Is it plugged in?");
            ImGui::Separator();

            if (ImGui::Button("Retry", ImVec2(120, 0)))
            {
                return true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit", ImVec2(120, 0)))
            {
                return false;
            }

            ImGui::EndPopup();
        }

        ImGui::End();
        ImGui::Render();
        glfwSwapBuffers(window);
    }
    return false;
}


GLuint EmptyTexture()                           // Create An Empty Texture

{

    GLuint txtnumber;                       // Texture ID

    unsigned int* data;                     // Stored Data



    // Create Storage Space For Texture Data (128x128x4)

    data = (unsigned int*)new GLuint[((1024 * 1024) * 4 * sizeof(unsigned int))];
    memset(data, 0, ((1024 * 1024) * 4 * sizeof(unsigned int)));   // Clear Storage Memory



    glGenTextures(1, &txtnumber);                   // Create 1 Texture

    glBindTexture(GL_TEXTURE_2D, txtnumber);            // Bind The Texture

    glTexImage2D(GL_TEXTURE_2D, 0, 4, 1024, 1024, 0,

        GL_RGBA, GL_UNSIGNED_BYTE, data);           // Build Texture Using Information In data

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);



    delete[] data;                         // Release data



    return txtnumber;                       // Return The Texture ID

}

void DrawAxis()
{
    //// Set Top Point Of Triangle To Red
    //glColor3f(1.0f, 0.0f, 0.0f);
    //glBegin(GL_TRIANGLES);                  // Start Drawing A Triangle
    //    glVertex3f(0.0f, 1.0f, 0.0f);       // First Point Of The Triangle
    //    glColor3f(0.0f, 1.0f, 0.0f);        // Set Left Point Of Triangle To Green
    //    glVertex3f(-1.0f, -1.0f, 0.0f);     // Second Point Of The Triangle
    //    glColor3f(0.0f, 0.0f, 1.0f);        // Set Right Point Of Triangle To Blue
    //    glVertex3f(1.0f, -1.0f, 0.0f);      // Third Point Of The Triangle
    //glEnd();

    // Traingles For X axis
    glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex3f(1.1f, 0.0f, 0.0f);
        glVertex3f(1.0f, 0.05f, 0.0f);
        glVertex3f(1.0f, -0.05f, 0.0f);
    glEnd();

    // Traingles For Y axis
    glBegin(GL_TRIANGLES);
        glColor3f(0.0f, 1.0f, 0.0f);
        glVertex3f(0.0f, -1.1f, 0.0f);
        glVertex3f(0.0f, -1.0f, 0.05f);
        glVertex3f(0.0f, -1.0f, -0.05f);
    glEnd();
    glBegin(GL_TRIANGLES);
        glColor3f(0.0f, 1.0f, 0.0f);
        glVertex3f(0.0f, -1.1f, 0.0f);
        glVertex3f(0.05f, -1.0f, 0.0f);
        glVertex3f(-0.05f, -1.0f, 0.0f);
    glEnd();

    // Traingles For Z axis
    glBegin(GL_TRIANGLES);
        glColor3f(0.0f, 0.0f, 1.0f);
        glVertex3f(0.0f, 0.0f, 1.1f);
        glVertex3f(0.0f, 0.05f, 1.0f);
        glVertex3f(0.0f, -0.05f, 1.0f);
    glEnd();

    auto axisWidth = 4;
    glLineWidth(axisWidth);

    // Drawing Axis
    glBegin(GL_LINES);
        // X axis - Red
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex3f(0.0f, 0.0f, 0.0f);
        glVertex3f(1.0f, 0.0f, 0.0f);

        // Y axis - Green
        glColor3f(0.0f, 1.0f, 0.0f);
        glVertex3f(0.0f, 0.0f, 0.0f);
        glVertex3f(0.0f, -1.0f, 0.0f);

        // Z axis - White
        glColor3f(0.0f, 0.0f, 1.0f);
        glVertex3f(0.0f, 0.0f, 0.0f);
        glVertex3f(0.0f, 0.0f, 1.0f);
    glEnd();
}

void DrawCyrcle(float xx, float xy, float xz, float yx, float yy, float yz)
{
    const auto N = 50;
    glColor3f(0.5f, 0.5f, 0.5f);
    glLineWidth(2);
    glBegin(GL_LINE_STRIP);


    for (int i = 0; i <= N; i++)
    {
        const auto theta = (2 * M_PI / N) * i;
        const auto cost = cos(theta);
        const auto sint = sin(theta);
        glVertex3f(
            1.1 * (xx * cost + yx * sint),
            1.1 * (xy * cost + yy * sint),
            1.1 * (xz * cost + yz * sint)
            );
    }

    glEnd();
}

int main(int, char**) try
{
    // activate logging to console
    rs::log_to_console(RS_LOG_SEVERITY_WARN);

    // Init GUI
    if (!glfwInit())
        exit(1);

    // Create GUI Windows
    auto window = glfwCreateWindow(1280, 720, "librealsense - config-ui", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    ImGui_ImplGlfw_Init(window, true);

    ImVec4 clear_color = ImColor(10, 0, 0);

    // Create RealSense Context
    rs::context ctx;
    auto device_index = 0;
    auto list = ctx.query_devices(); // Query RealSense connected devices list

    // If no device is connected...
    while (list.size() == 0)
    {
        if (!no_device_popup(window, clear_color)) return EXIT_SUCCESS;

        list = ctx.query_devices();
    }

    auto dev = list[device_index];
    std::vector<std::string> device_names;

    std::string error_message = "";

    for (auto&& l : list)
    {
        auto d = list[device_index];
        auto name = d.get_camera_info(RS_CAMERA_INFO_DEVICE_NAME);
        auto serial = d.get_camera_info(RS_CAMERA_INFO_DEVICE_SERIAL_NUMBER);
        device_names.push_back(to_string() << name << " Sn#" << serial);
    }

    auto model = device_model(dev, error_message);
    std::string label;

    GLuint tex_id = EmptyTexture();

    float a = 0;
    float x = 10;
    float y = 10;
    float z = 20;

    while (!glfwWindowShouldClose(window))
    {
        a += 0.1;
        glfwPollEvents();
        int w, h;
        glfwGetWindowSize(window, &w, &h);

        glViewport(0, 0, 1024, 1024);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);                        // Select The Projection Matrix
        glLoadIdentity();                                   // Reset The Projection Matrix

        // Calculate The Aspect Ratio Of The Window
        //gluPerspective(45.0f, 256 / 256, 0.1f, 100.0f);
        //gluPerspective(45.0f, 256 / 256, 4.0f, 10.0f);
        glOrtho(-2.3, 2.3, -1.8, 1.8, -7, 7);

        glRotatef(-35, 1.0f, 0.0f, 0.0f);

        glTranslatef(0, 0.33f, -1.f);

        float normal = (1 / std::sqrt(x*x + y*y + z*z));

        
        glRotatef(-45, 0.0f, 1.0f, 0.0f);

        DrawAxis();
        DrawCyrcle(1, 0, 0, 0, 1, 0); 
        DrawCyrcle(0, 1, 0, 0, 0, 1);
        DrawCyrcle(1, 0, 0, 0, 0, 1);
        
        
        auto vectorWidth = 5;
        glLineWidth(vectorWidth);
        glBegin(GL_LINES);
            glColor3f(1.0f, 1.0f, 1.0f);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(normal *x, normal *y, normal *z);
        glEnd();


        glBindTexture(GL_TEXTURE_2D, tex_id);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 1024, 1024, 0);


        ImGui_ImplGlfw_NewFrame();

        auto flags = ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse;

        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 300, static_cast<float>(h) });
        ImGui::Begin("Control Panel", nullptr, flags);

        ImGui::Text("Viewer FPS: %.0f ", ImGui::GetIO().Framerate);

        if (ImGui::CollapsingHeader("Device Details", nullptr, true, true))
        {
            // Draw a combo-box with the list of connected devices
            auto device_names_chars = get_string_pointers(device_names);
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("", &device_index, device_names_chars.data(),
                static_cast<int>(device_names.size())))
            {
                for (auto&& sub : model.subdevices)
                {
                    if (sub->streaming)
                        sub->stop();
                }

                dev = list[device_index];
                model = device_model(dev, error_message);
            }
            ImGui::PopItemWidth();

            for (auto i = 0; i < RS_CAMERA_INFO_COUNT; i++)
            {
                auto info = static_cast<rs_camera_info>(i);
                if (dev.supports(info))
                {
                    std::stringstream ss;
                    ss << rs_camera_info_to_string(info) << ":";
                    auto line = ss.str();
                    ImGui::PushStyleColor(ImGuiCol_Text, { 1.0f, 1.0f, 1.0f, 0.5f });
                    ImGui::Text(line.c_str());
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    auto value = dev.get_camera_info(info);
                    ImGui::Text(value);

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(value);
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Streaming", nullptr, true, true))
        {
            for (auto&& sub : model.subdevices)
            {
                label = to_string() << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME);
                if (ImGui::CollapsingHeader(label.c_str(), nullptr, true, true))
                {
                    auto res_chars = get_string_pointers(sub->resolutions);
                    auto fps_chars = get_string_pointers(sub->fpses);

                    ImGui::Text("Resolution:");
                    ImGui::SameLine();
                    ImGui::PushItemWidth(-1);
                    label = to_string() << sub->dev.get_camera_info(RS_CAMERA_INFO_DEVICE_NAME)
                                        << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME) << " resolution";
                    if (sub->streaming) ImGui::Text(res_chars[sub->selected_res_id]);
                    else ImGui::Combo(label.c_str(), &sub->selected_res_id, res_chars.data(),
                                      static_cast<int>(res_chars.size()));
                    ImGui::PopItemWidth();

                    ImGui::Text("FPS:");
                    ImGui::SameLine();
                    ImGui::PushItemWidth(-1);
                    label = to_string() << sub->dev.get_camera_info(RS_CAMERA_INFO_DEVICE_NAME)
                                        << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME) << " fps";
                    if (sub->streaming) ImGui::Text(fps_chars[sub->selected_fps_id]);
                    else ImGui::Combo(label.c_str(), &sub->selected_fps_id, fps_chars.data(),
                                      static_cast<int>(fps_chars.size()));
                    ImGui::PopItemWidth();

                    auto live_streams = 0;
                    for (auto i = 0; i < RS_STREAM_COUNT; i++)
                    {
                        auto stream = static_cast<rs_stream>(i);
                        if (sub->formats[stream].size() > 0) live_streams++;
                    }

                    for (auto i = 0; i < RS_STREAM_COUNT; i++)
                    {
                        auto stream = static_cast<rs_stream>(i);
                        if (sub->formats[stream].size() == 0) continue;
                        auto formats_chars = get_string_pointers(sub->formats[stream]);
                        if (live_streams > 1)
                        {
                            label = to_string() << rs_stream_to_string(stream) << " format:";
                            if (sub->streaming) ImGui::Text(label.c_str());
                            else ImGui::Checkbox(label.c_str(), &sub->stream_enabled[stream]);
                        }
                        else
                        {
                            label = to_string() << "Format:";
                            ImGui::Text(label.c_str());
                        }

                        ImGui::SameLine();
                        if (sub->stream_enabled[stream])
                        {
                            ImGui::PushItemWidth(-1);
                            label = to_string() << sub->dev.get_camera_info(RS_CAMERA_INFO_DEVICE_NAME)
                                                << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME)
                                                << " " << rs_stream_to_string(stream) << " format";
                            if (sub->streaming) ImGui::Text(formats_chars[sub->selected_format_id[stream]]);
                            else ImGui::Combo(label.c_str(), &sub->selected_format_id[stream], formats_chars.data(),
                                static_cast<int>(formats_chars.size()));
                            ImGui::PopItemWidth();
                        }
                        else
                        {
                            ImGui::Text("N/A");
                        }
                    }

                    try
                    {
                        if (!sub->streaming)
                        {
                            label = to_string() << "Play " << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME);

                            if (sub->is_selected_combination_supported())
                            {
                                if (ImGui::Button(label.c_str()))
                                {
                                    sub->play(sub->get_selected_profiles());
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled(label.c_str());
                            }
                        }
                        else
                        {
                            label = to_string() << "Stop " << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME);
                            if (ImGui::Button(label.c_str()))
                            {
                                sub->stop();
                            }
                        }
                    }
                    catch(const rs::error& e)
                    {
                        error_message = error_to_string(e);
                    }
                    catch(const std::exception& e)
                    {
                        error_message = e.what();
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Control", nullptr, true, true))
        {
            for (auto&& sub : model.subdevices)
            {
                label = to_string() << sub->dev.get_camera_info(RS_CAMERA_INFO_MODULE_NAME) << " options:";
                if (ImGui::CollapsingHeader(label.c_str(), nullptr, true, false))
                {
                    for (auto i = 0; i < RS_OPTION_COUNT; i++)
                    {
                        auto opt = static_cast<rs_option>(i);
                        auto&& metadata = sub->options_metadata[opt];
                        metadata.draw(error_message);
                    }
                }
            }
        }

        for (auto&& sub : model.subdevices)
        {
            sub->update(error_message);
        }

        if (error_message != "")
            ImGui::OpenPopup("Oops, something went wrong!");
        if (ImGui::BeginPopupModal("Oops, something went wrong!", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("RealSense error calling:");
            ImGui::InputTextMultiline("error", const_cast<char*>(error_message.c_str()),
                error_message.size(), { 500,100 }, ImGuiInputTextFlags_AutoSelectAll);
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(120, 0)))
            {
                error_message = "";
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::End();

        // Fetch frames from queue
        for (auto&& sub : model.subdevices)
        {
            for (auto& queue : sub->queues)
            {
                rs::frame f;
                if (queue->poll_for_frame(&f))
                {
                    model.stream_buffers[f.get_stream_type()].upload(f);
                    model.steam_last_frame[f.get_stream_type()] = std::chrono::high_resolution_clock::now();
                    auto width = (f.get_format() == RS_FORMAT_MOTION_DATA)? 640 : f.get_width();
                    auto height = (f.get_format() == RS_FORMAT_MOTION_DATA)? 480 : f.get_height();
                    model.stream_size[f.get_stream_type()] = {width, height};
                    model.stream_format[f.get_stream_type()] = f.get_format();
                }
            }

        }

        // Rendering
        glViewport(0, 0,
            static_cast<int>(ImGui::GetIO().DisplaySize.x),
            static_cast<int>(ImGui::GetIO().DisplaySize.y));
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        glfwGetWindowSize(window, &w, &h);
        glLoadIdentity();
        glOrtho(0, w, h, 0, -1, +1);

        auto layout = model.calc_layout(300, 0, w - 300, h);

        for (auto kvp : layout)
        {
            auto&& view_rect = kvp.second;
            auto stream = kvp.first;
            auto&& stream_size = model.stream_size[stream];
            auto stream_rect = view_rect.adjust_ratio(stream_size);

            model.stream_buffers[stream].show(stream_rect, model.get_stream_alpha(stream));

            flags = ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoTitleBar;

            ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0, 0, 0, 0 });
            ImGui::SetNextWindowPos({ stream_rect.x, stream_rect.y });
            ImGui::SetNextWindowSize({ stream_rect.w, stream_rect.h });
            label = to_string() << "Stream of " << rs_stream_to_string(stream);
            ImGui::Begin(label.c_str(), nullptr, flags);

            label = to_string() << rs_stream_to_string(stream) << " "
                << stream_size.x << "x" << stream_size.y << ", "
                << rs_format_to_string(model.stream_format[stream]);

            if (layout.size() > 1 && !model.fullscreen)
            {
                ImGui::Text(label.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 30);
                if (ImGui::Button("[+]", { 26, 20 }))
                {
                    model.fullscreen = true;
                    model.selected_stream = stream;
                }
            } 
            else if (model.fullscreen)
            {
                ImGui::Text(label.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 30);
                if (ImGui::Button("[-]", { 26, 20 }))
                {
                    model.fullscreen = false;
                }
            }

            ImGui::End();
            ImGui::PopStyleColor();
        }

        ImGui::Render();
        glfwSwapBuffers(window);
    }

    for (auto&& sub : model.subdevices)
    {
        if (sub->streaming)
            sub->stop();
    }

    // Cleanup
    ImGui_ImplGlfw_Shutdown();
    glfwTerminate();

    return EXIT_SUCCESS;
}
catch (const rs::error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
