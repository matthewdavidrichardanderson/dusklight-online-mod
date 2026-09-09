#include "dusklight_online/net/reliable_json.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>
using namespace dusklight_online::net;
using json = nlohmann::json;
int main(int argc, char** argv) {
    auto rejects = [](const std::string& bytes) {
        bool rejected = false;
        try { (void)decode_reliable_json(bytes); } catch (...) { rejected = true; }
        assert(rejected);
    };
    json small = {{"type", "pvp_hit"}, {"damage", 4}};
    assert(encode_reliable_json(small) == small.dump());
    assert(decode_reliable_json(small.dump()) == small);
    json big = {{"type", "save_snapshot"}, {"state", std::string(200000, 'x')}};
    auto packed = encode_reliable_json(big);
    assert(packed.starts_with("Z1") && packed.size() < 1000);
    assert(decode_reliable_json(packed) == big);
    rejects(packed.substr(0, packed.size()-2));
    rejects(packed + "00");
    rejects("Z1"); rejects("Z1000"); rejects("Z1gg"); rejects("Z10000");
    rejects(std::string(reliableJsonLimit + 1, 'x'));
    // A valid compressed frame advertising expansion beyond the allowed bound.
    std::string oversized(reliableJsonLimit + 1, 'x');
    std::string compressed(ZSTD_compressBound(oversized.size()), '\0');
    auto count = ZSTD_compress(compressed.data(), compressed.size(), oversized.data(), oversized.size(), 1);
    std::string bomb="Z1"; const char* hex="0123456789abcdef";
    for (size_t i=0;i<count;++i) { unsigned char c=compressed[i]; bomb+=hex[c>>4]; bomb+=hex[c&15]; }
    rejects(bomb);
    std::mt19937 rng(42);
    std::string noise(10000, ' ');
    for (auto& c:noise) c=char(32+rng()%95);
    json random={{"noise",noise}};
    assert(decode_reliable_json(encode_reliable_json(random))==random);
    assert(argc==2);
    // The transport-only fixture contains a Zstd/base64-encoded 3996-byte
    // synthetic save with a poorly compressible body, not repeated padding.
    // It exercises nested compression and must never be applied as a game save.
    std::ifstream file(argv[1]); json progression; file>>progression;
    auto encoded=encode_reliable_json(progression);
    assert(decode_reliable_json(encoded)==progression);
    std::cout<<"Full progression: "<<progression.dump().size()+1<<" -> "<<encoded.size()+1<<" wire bytes; exact round trip passed\n";
}
