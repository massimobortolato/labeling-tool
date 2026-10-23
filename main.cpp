#include "easy_webview.h"
#include "example_static.h"
#include <windows.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

using easy_webview::handler;
using easy_webview::request;
using easy_webview::response;
using easy_webview::webview;
using json = nlohmann::json;

namespace fs = std::filesystem;

static fs::path utf8_to_path(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return fs::path(w);
}

static fs::path pick_folder()
{
    fs::path out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->Show(nullptr)))
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                {
                    out = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

static fs::path g_images_dir;

static std::string path_to_utf8(const fs::path &p)
{
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

static const char *image_mime(const fs::path &p)
{
    std::string ext = path_to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".png")
        return "image/png";
    if (ext == ".bmp")
        return "image/bmp";
    if (ext == ".gif")
        return "image/gif";
    if (ext == ".webp")
        return "image/webp";
    return nullptr;
}

// Apre il selettore nativo, ricorda la cartella e ne elenca le immagini (percorsi relativi).
static response open_folder(const request &)
{
    fs::path dir = pick_folder();
    if (dir.empty())
        return {200, "text/plain; charset=utf-8", "CANCELLED"};
    g_images_dir = dir;

    json images = json::array();
    std::error_code ec;
    for (const auto &e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        if (e.is_regular_file(ec) && image_mime(e.path()))
            images.push_back(path_to_utf8(fs::relative(e.path(), dir)));

    json out = {{"dir", path_to_utf8(dir)}, {"images", images}};
    return {200, "application/json; charset=utf-8", out.dump()};
}

// GET /api/image?name=<percorso relativo>: serve solo file immagine dentro la cartella aperta.
static response serve_image(const request &req)
{
    const response not_found{404, "text/plain; charset=utf-8", "not found"};
    auto it = req.query.find("name");
    if (it == req.query.end() || g_images_dir.empty())
        return not_found;

    fs::path full = fs::weakly_canonical(g_images_dir / utf8_to_path(it->second));
    fs::path rel = full.lexically_relative(fs::weakly_canonical(g_images_dir));
    const char *mime = image_mime(full);
    if (!mime || rel.empty() || *rel.begin() == ".." || !fs::is_regular_file(full))
        return not_found;

    std::ifstream f(full, std::ios::binary);
    std::ostringstream data;
    data << f.rdbuf();
    return {200, mime, data.str()};
}

// Body JSON: {"dir", "classes": [nome...], "images": [{"name", "w", "h", "rects": [{"cls", "pts": [[x, y] x4]}]}]}
static response save_labels(const request &req)
{
    auto text_response = [](int status, const std::string &body) {
        return response{status, "text/plain; charset=utf-8", body};
    };

    json j = json::parse(req.body, nullptr, false);
    if (j.is_discarded())
        return text_response(400, "invalid json");

    if (g_images_dir.empty() || j.value("dir", std::string()) != path_to_utf8(g_images_dir))
        return text_response(400, "images folder not opened");
    const fs::path dir = g_images_dir;

    try
    {
        fs::create_directories(dir / "labels");
        int count = 0;
        for (const auto &img : j.at("images"))
        {
            const std::string name = img.at("name").get<std::string>();
            const double w = img.at("w").get<double>(), h = img.at("h").get<double>();
            fs::path file = utf8_to_path(name + ".txt");
            bool bad = name.empty() || w <= 0 || h <= 0 || name.find(':') != std::string::npos || !file.is_relative();
            for (const auto &part : file)
                if (part == "..")
                    bad = true;
            if (bad)
                return text_response(400, "invalid image entry");

            std::ostringstream out;
            out << std::fixed << std::setprecision(6);
            for (const auto &r : img.at("rects"))
            {
                out << r.at("cls").get<int>();
                for (const auto &p : r.at("pts"))
                    out << ' ' << std::clamp(p.at(0).get<double>() / w, 0.0, 1.0) << ' '
                        << std::clamp(p.at(1).get<double>() / h, 0.0, 1.0);
                out << '\n';
            }
            const fs::path target = dir / "labels" / file;
            fs::create_directories(target.parent_path());
            std::ofstream(target, std::ios::binary) << out.str();
            ++count;
        }

        std::ostringstream yaml;
        yaml << "path: .\ntrain: .\nval: .\nnames:\n";
        int i = 0;
        for (const auto &c : j.at("classes"))
            yaml << "  " << i++ << ": " << c.dump() << '\n';
        std::ofstream(dir / "data.yaml", std::ios::binary) << yaml.str();

        const auto u8 = dir.u8string();
        return text_response(200, "OK " + std::to_string(count) + " " + std::string(u8.begin(), u8.end()));
    }
    catch (const std::exception &e)
    {
        return text_response(400, e.what());
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
// int main()
{

    webview w2("YOLO cpp", true);
    w2.serve_static(example_static);
    // w2.serve_static("../../static");
    w2.post("/api/open", open_folder);
    w2.get("/api/image", serve_image);
    w2.post("/api/save", save_labels);

    webview::run();
    return 0;
}
