// lookmatch-render: drive the LookMatch engine from the command line.
// Same engine the Premiere effect uses, so renders here match the plugin.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include "../core/engine.h"
#include "../core/matcher.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace lm;

struct Frame {
    int w = 0, h = 0;
    std::vector<float> px;  // RGBA
    ImageView view() { return {px.data(), w, h, w * 4}; }
};

static bool load(const std::string& path, Frame& img) {
    int n;
    stbi_us* d = stbi_load_16(path.c_str(), &img.w, &img.h, &n, 4);
    if (!d) { std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(), stbi_failure_reason()); return false; }
    img.px.resize(size_t(img.w) * img.h * 4);
    for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = d[i] / 65535.0f;
    stbi_image_free(d);
    return true;
}

static bool save(const std::string& path, const Frame& img) {
    std::vector<unsigned char> b(img.px.size());
    for (size_t i = 0; i < b.size(); ++i) b[i] = (unsigned char)(clamp01(img.px[i]) * 255.0f + 0.5f);
    bool jpg = path.size() > 4 && (path.substr(path.size() - 4) == ".jpg" || path.substr(path.size() - 5) == ".jpeg");
    int ok = jpg ? stbi_write_jpg(path.c_str(), img.w, img.h, 4, b.data(), 95)
                 : stbi_write_png(path.c_str(), img.w, img.h, 4, b.data(), img.w * 4);
    if (!ok) std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return ok != 0;
}

static void usage() {
    std::printf(
        "lookmatch-render --in FRAME --out OUT.png [options]\n\n"
        "Look match\n"
        "  --ref IMAGE            reference still to match (the footage frame is --in); several stills of one\n"
        "                         look: --ref \"a.png|b.png|c.png\" (combined by median)\n"
        "  --match-strength F     0..1 (1)\n"
        "  --region-strength F    0..1 (1)    --skin-protect F  0..1 (auto)\n"
        "  --source NAME          footage encoding for the match (S-Log3, LogC3, V-Log, ...; default: detect)\n"
        "  --norm-exposure F --norm-white F --norm-contrast F --look-strength F --match-contrast F   0..1 (1)\n"
        "Input\n"
        "  --input NAME           rec709 | logc3 | slog3 | vlog (rec709)\n"
        "  --exposure F --temperature F --tint F\n"
        "Film\n"
        "  --film F               film emulation strength 0..1 (0)\n"
        "  --negative N --print N stock indices, see --list-stocks\n"
        "  --printer R,G,B        printer points\n"
        "  --film-sat F           film saturation (1)\n"
        "  --fade F               black lift 0..1\n"
        "  --halation F [--halation-radius F --halation-threshold F]\n"
        "  --bloom F [--bloom-radius F --bloom-threshold F]\n"
        "  --grain F [--grain-size F --grain-color F]\n"
        "  --softness F --vignette F --weave PX --flicker F\n"
        "Output\n"
        "  --mix F                0..1 (1)\n"
        "  --deband F             smooth the source's quantisation steps before the grade, 0..1 (0)\n"
        "  --skin-evening F       pull skin colour toward the skin around it, with a match, 0..1 (0.5)\n"
        "  --frame N              frame number for grain / weave / flicker (0)\n"
        "  --sequence N           render N frames as OUT_0000.png ... (shows grain, weave, flicker)\n"
        "  --export-lut PATH      also write the colour part as a .cube for Lumetri\n"
        "  --lut-size N           export LUT size (65)\n"
        "  --list-stocks          print film stocks\n");
}

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) { usage(); return 1; }
        k = k.substr(2);
        if (k == "list-stocks" || k == "help" || k == "dump-transfers") { a[k] = "1"; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --%s\n", k.c_str()); return 1; }
        a[k] = argv[++i];
    }
    if (a.count("help")) { usage(); return 0; }
    if (a.count("dump-transfers")) {  // for tests/check_transfers.py: code -> linear, and gamut -> Rec.709
        std::printf("{");
        for (int t = 0; t < kTransferCount; ++t) {
            std::printf("%s\"%s\": {\"decode\": [", t ? ", " : "", transferShortName(Transfer(t)));
            for (int i = 0; i <= 64; ++i) std::printf("%s%.9g", i ? ", " : "", transferDecode(Transfer(t), i / 64.0f));
            const Mat3 m = gamutToRec709(Transfer(t));
            std::printf("], \"toRec709\": [[%.7g, %.7g, %.7g], [%.7g, %.7g, %.7g], [%.7g, %.7g, %.7g]]}", m.m[0][0], m.m[0][1], m.m[0][2],
                        m.m[1][0], m.m[1][1], m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2]);
        }
        std::printf("}\n");
        return 0;
    }
    if (a.count("list-stocks")) {
        std::printf("Negatives:\n");
        for (int i = 0; i < negativeStockCount(); ++i) std::printf("  %d  %s\n", i, negativeStock(i).name);
        std::printf("Prints:\n");
        for (int i = 0; i < printStockCount(); ++i) std::printf("  %d  %s\n", i, printStock(i).name);
        return 0;
    }
    if (!a.count("in") || !a.count("out")) { usage(); return 1; }
    auto F = [&](const char* k, float d) { return a.count(k) ? std::strtof(a[k].c_str(), nullptr) : d; };
    auto I = [&](const char* k, int d) { return a.count(k) ? std::atoi(a[k].c_str()) : d; };

    Params p;
    std::string in = a.count("input") ? a["input"] : "rec709";
    p.input = in == "logc3" ? InputSpace::ArriLogC3 : in == "slog3" ? InputSpace::SonySLog3 : in == "vlog" ? InputSpace::PanaVLog : InputSpace::Rec709;
    p.exposure = F("exposure", 0); p.temperature = F("temperature", 0); p.tint = F("tint", 0);
    p.matchStrength = F("match-strength", 1);
    p.filmStrength = F("film", 0);
    p.film.negative = I("negative", 0); p.film.print = I("print", 1); p.film.saturation = F("film-sat", 1);
    if (a.count("printer")) std::sscanf(a["printer"].c_str(), "%f,%f,%f", &p.film.printerPoints[0], &p.film.printerPoints[1], &p.film.printerPoints[2]);
    p.fade = F("fade", 0);
    p.halation = F("halation", 0); p.halationRadius = F("halation-radius", 1); p.halationThreshold = F("halation-threshold", 0.6f);
    p.bloom = F("bloom", 0); p.bloomRadius = F("bloom-radius", 1); p.bloomThreshold = F("bloom-threshold", 0.5f);
    p.grain = F("grain", 0); p.grainSize = F("grain-size", 1); p.grainColor = F("grain-color", 0.3f);
    p.softness = F("softness", 0); p.vignette = F("vignette", 0); p.weave = F("weave", 0); p.flicker = F("flicker", 0);
    p.mix = F("mix", 1);
    p.deband = F("deband", 0);
    p.skinEven = F("skin-evening", 0.5f);

    Frame src;
    if (!load(a["in"], src)) return 1;
    Engine engine;
    engine.setParams(p);

    if (a.count("ref")) {
        // one still, or several stills of one look separated by '|'
        std::vector<Frame> refFrames;
        {
            std::string all = a["ref"];
            size_t start = 0;
            while (start <= all.size()) {
                size_t end = all.find('|', start);
                std::string one = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                start = end == std::string::npos ? all.size() + 1 : end + 1;
                if (one.empty()) continue;
                refFrames.emplace_back();
                if (!load(one, refFrames.back())) return 1;
            }
        }
        auto t0 = std::chrono::steady_clock::now();
        SceneMatchOptions mo;
        mo.look.regionStrength = F("region-strength", 1);
        mo.look.skinProtect = F("skin-protect", -1);
        mo.transfer = a.count("source") ? transferFromName(a["source"]) : -1;
        if (mo.transfer < 0) {
            Image full; full.width = src.w; full.height = src.h;
            for (size_t i = 0; i < src.px.size(); i += 4) full.px.push_back({src.px[i], src.px[i + 1], src.px[i + 2]});
            if (isLog(detectTransfer(downsample(full, 480)).transfer)) {
                std::vector<Image> more;  // more frames of the clip for the detector: --detect-frames "a.png|b.png"
                std::vector<const Image*> all{&full};
                if (a.count("detect-frames")) {
                    std::stringstream list(a["detect-frames"]);
                    std::string one;
                    while (std::getline(list, one, '|')) {
                        Frame f;
                        if (!load(one, f)) return 1;
                        Image im; im.width = f.w; im.height = f.h;
                        for (size_t i = 0; i < f.px.size(); i += 4) im.px.push_back({f.px[i], f.px[i + 1], f.px[i + 2]});
                        more.push_back(std::move(im));
                    }
                    more.shrink_to_fit();
                    for (const Image& m : more) all.push_back(&m);
                }
                const LogRank rank = rankLogCurves(all);
                mo.transfer = int(rank.transfer);
                mo.transferGuess = rank.label();
                std::fprintf(stderr, "log detector: %s\n", rank.label().c_str());
                for (size_t i = 0; i < rank.ranked.size() && i < 4; ++i) std::fprintf(stderr, "  %s %.0f%%\n", transferShortName(rank.ranked[i].first), rank.ranked[i].second * 100);
            }
        }
        mo.exposure = F("norm-exposure", 1); mo.whiteBalance = F("norm-white", 1);
        mo.contrast = F("norm-contrast", 1); mo.lookStrength = F("look-strength", 1);
        mo.matchContrast = F("match-contrast", 1);
        auto image = [](const Frame& f, const std::function<Vec3(Vec3)>& g) {
            lm::Image im;
            im.width = f.w; im.height = f.h;
            im.px.resize(size_t(f.w) * f.h);
            for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = g({f.px[i * 4], f.px[i * 4 + 1], f.px[i * 4 + 2]});
            return im;
        };
        std::vector<lm::Image> refs;
        for (const Frame& f : refFrames) refs.push_back(image(f, [](Vec3 c) { return c; }));
        SceneMatch m = matchScene(refs, image(src, [&](Vec3 c) { return engine.neutralDisplay(c); }), mo);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("%smatch built in %.0f ms\nsource %s\nresult %s\n", m.report.c_str(),
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), m.source.json().c_str(), m.result.json().c_str());
        engine.setMatchLut(std::make_shared<Lut3D>(std::move(m.lut)));
    }

    if (a.count("export-lut")) {
        Lut3D l = engine.bakeExportLut(I("lut-size", 65));
        if (!l.writeCube(a["export-lut"], "LookMatch")) { std::fprintf(stderr, "cannot write %s\n", a["export-lut"].c_str()); return 1; }
        std::printf("wrote %s (colour only: halation, bloom, grain, weave and vignette need the plugin)\n", a["export-lut"].c_str());
    }

    int frames = I("sequence", 1);
    long frame0 = I("frame", 0);
    Frame dst = src;
    for (int f = 0; f < frames; ++f) {
        auto t0 = std::chrono::steady_clock::now();
        engine.render(src.view(), dst.view(), frame0 + f);
        auto t1 = std::chrono::steady_clock::now();
        std::string out = a["out"];
        if (frames > 1) {
            char suffix[16];
            std::snprintf(suffix, sizeof suffix, "_%04d", f);
            size_t dot = out.find_last_of('.');
            out = out.substr(0, dot) + suffix + out.substr(dot);
        }
        if (!save(out, dst)) return 1;
        std::printf("frame %ld %dx%d rendered in %.1f ms -> %s\n", frame0 + f, src.w, src.h,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), out.c_str());
    }
    return 0;
}
