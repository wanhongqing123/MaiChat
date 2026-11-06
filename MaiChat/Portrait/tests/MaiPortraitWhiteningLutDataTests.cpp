// Compile this file alone. Including the implementation intentionally exercises
// the production asset decoder without exposing private LUT bytes in its API.
#include "../MaiPortraitWhiteningLut.cpp"

#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const Lookups lookup;
    std::ofstream output(argv[1], std::ios::binary);
    for (const Lookup* asset : {&lookup.gray, &lookup.origin, &lookup.skin, &lookup.light}) {
        output.write(reinterpret_cast<const char*>(asset->rgb.data()), asset->rgb.size());
    }
    return output.good() ? 0 : 3;
}
