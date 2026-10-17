#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvgrast.h"

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    try {
        for (const char *properties : {"fill", "", " ", "fill:red; ", "class:a;fill:red",
                                       "style:class:a;fill:red", "class:b;fill:red"}) {
            std::string source = "<svg width='16' height='8'><style>.a{" + std::string(properties) +
                                 "}.b{class:a;}</style><rect class='a' width='16' height='8'/></svg>";
            std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(source.data(), "px", 96),
                                                                    nsvgDelete);
            require(image && image->shapes, "malformed CSS must not crash or recurse");
            if (std::string(properties).find("fill:red") != std::string::npos)
                require((image->shapes->fill.color & 0xffffff) == 0xff, "ordinary CSS fill remains intact");
        }
        for (const char *transform : {"matrix()", "translate()", "scale()", "rotate()", "rotate(1 2)",
                                      "skewX()", "skewY()", "translate(3) scale()"}) {
            float matrix[6];
            nsvg__parseTransform(matrix, transform);
            for (float value : matrix)
                require(std::isfinite(value), "malformed transforms remain finite");
            require(matrix[0] == 1 && matrix[3] == 1 && matrix[1] == 0 && matrix[2] == 0 && matrix[5] == 0,
                    "malformed transforms are ignored");
            require(matrix[4] == (std::string(transform).starts_with("translate(3)") ? 3 : 0),
                    "valid neighboring transforms remain intact");
        }
        float singular[6] = {}, inverse[6] = {};
        nsvg__xformInverse(inverse, singular);
        require(inverse[0] == 1 && inverse[3] == 1 && singular[0] == 0,
                "singular inverse initializes output without changing input");
        for (const char *dash : {"0.000002", "0.0001", "0 2 3"}) {
            std::string source = "<svg width='200' height='200'><path d='M100 100 L199 100' stroke='black' "
                                 "stroke-dasharray='" +
                                 std::string(dash) + "'/></svg>";
            std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(source.data(), "px", 96),
                                                                    nsvgDelete);
            std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> raster(nsvgCreateRasterizer(),
                                                                                    nsvgDeleteRasterizer);
            require(image && raster, "SVG test allocation");
            std::vector<unsigned char> pixels(200 * 200 * 4);
            nsvgRasterize(raster.get(), image.get(), 0, 0, 1, pixels.data(), 200, 200, 800);
            require(bool(raster->failed) == (std::string(dash) != "0 2 3"),
                    "tiny dashes are bounded while ordinary zero/positive dashes render");
        }
        std::cout << "PASS: SVG CSS bounds, cycles, transforms and dash work limits\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
