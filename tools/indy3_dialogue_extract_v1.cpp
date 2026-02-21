/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This standalone tool extracts dialogue strings from Indy3 (SCUMM v3).
 * Parsing behavior is inspired by ScummVM engine logic in:
 * - engines/scumm/resource.cpp (small-header chunk walking)
 * - engines/scumm/resource_v4.cpp (00.LFL index directories)
 * - engines/scumm/script.cpp (getVerbEntrypoint, resStrLen)
 * - engines/scumm/script_v5.cpp (o5_print/o5_printEgo/decodeParseString)
 */

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

#ifdef INDY3_DIALOGUE_EXTRACT_V2
constexpr bool kEmitSpeakerLabel = true;
#else
constexpr bool kEmitSpeakerLabel = false;
#endif

#ifdef INDY3_DIALOGUE_EXTRACT_V3
constexpr bool kEnableRecoveredPrintPass = true;
constexpr bool kEnableMissingDialogueAudit = true;
#else
constexpr bool kEnableRecoveredPrintPass = false;
constexpr bool kEnableMissingDialogueAudit = false;
#endif

constexpr uint16_t TAG_RO = 0x4F52;
constexpr uint16_t TAG_SC = 0x4353;
constexpr uint16_t TAG_OC = 0x434F;
constexpr uint16_t TAG_EN = 0x4E45;
constexpr uint16_t TAG_EX = 0x5845;
constexpr uint16_t TAG_LS = 0x534C;
constexpr uint16_t TAG_S0 = 0x5330;

struct Options {
    std::string gameDir;
    std::string outDir;
    bool splitWaits = false;
    bool dedupe = false;
    bool strict = false;
    bool includeRaw = false;
    bool verbose = false;
};

struct ChunkView {
    uint16_t tag = 0;
    uint32_t size = 0;
    uint32_t fileOffset = 0;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
};

enum class SourceKind {
    GlobalScriptSC,
    RoomEntryEN,
    RoomExitEX,
    LocalScriptLS,
    ObjectCodeOC,
    ObjectVerbScript
};

enum class SpeakerRefKind {
    Unknown,
    Ego,
    DirectActor,
    Variable
};

struct ScriptSource {
    SourceKind kind = SourceKind::GlobalScriptSC;
    std::string lflFile;
    int roomId = -1;
    int scriptId = -1;
    int objectId = -1;
    int verbId = -1;
    uint32_t baseOffsetFile = 0;
    uint32_t len = 0;
    std::shared_ptr<std::vector<uint8_t>> buffer;
};

struct ExtractedLine {
    std::string textRawHex;
    std::string textNormalized;
    std::string lflFile;
    SourceKind sourceKind = SourceKind::GlobalScriptSC;
    int roomId = -1;
    int scriptId = -1;
    int objectId = -1;
    int verbId = -1;
    uint32_t opcodeOffsetInSource = 0;
    uint32_t messageOffsetInSource = 0;
    uint8_t opcode = 0;
    bool fromPrintEgo = false;
    bool recovered = false;
    SpeakerRefKind speakerRefKind = SpeakerRefKind::Unknown;
    int speakerActorId = -1;
    int speakerVarId = -1;
};

struct MissingCandidate {
    std::string text;
    std::string lflFile;
};

struct ParsedCandidate {
    bool ok = false;
    uint32_t messageOffsetInSource = 0;
    uint32_t messageLen = 0;
    bool fromPrintEgo = false;
    uint8_t opcode = 0;
    SpeakerRefKind speakerRefKind = SpeakerRefKind::Unknown;
    int speakerActorId = -1;
    int speakerVarId = -1;
    bool recovered = false;
};

std::string sourceKindToString(SourceKind kind) {
    switch (kind) {
    case SourceKind::GlobalScriptSC:
        return "GlobalScriptSC";
    case SourceKind::RoomEntryEN:
        return "RoomEntryEN";
    case SourceKind::RoomExitEX:
        return "RoomExitEX";
    case SourceKind::LocalScriptLS:
        return "LocalScriptLS";
    case SourceKind::ObjectCodeOC:
        return "ObjectCodeOC";
    case SourceKind::ObjectVerbScript:
        return "ObjectVerbScript";
    }
    return "Unknown";
}

std::string speakerRefKindToString(SpeakerRefKind kind) {
    switch (kind) {
    case SpeakerRefKind::Unknown:
        return "unknown";
    case SpeakerRefKind::Ego:
        return "ego";
    case SpeakerRefKind::DirectActor:
        return "direct_actor";
    case SpeakerRefKind::Variable:
        return "variable";
    }
    return "unknown";
}

std::string speakerLabelFor(SpeakerRefKind kind, int actorId, int varId) {
    switch (kind) {
    case SpeakerRefKind::Ego:
        return "EGO";
    case SpeakerRefKind::DirectActor:
        if (actorId == 1) {
            return "INDY";
        }
        return "ACTOR_" + std::to_string(actorId);
    case SpeakerRefKind::Variable:
        return "VAR_" + std::to_string(varId);
    case SpeakerRefKind::Unknown:
    default:
        return "UNKNOWN";
    }
}

std::string trim(const std::string &value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string jsonEscape(const std::string &value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (c < 0x20) {
                out << "\\u00" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c) << std::dec << std::nouppercase;
            } else {
                out << static_cast<char>(c);
            }
            break;
        }
    }
    return out.str();
}

std::string hexByte(uint8_t b) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(b);
    return out.str();
}

std::string toLowerAscii(std::string value) {
    for (char &c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

uint16_t readLE16(const std::vector<uint8_t> &data, size_t pos) {
    return static_cast<uint16_t>(data[pos]) |
           static_cast<uint16_t>(data[pos + 1]) << 8;
}

uint32_t readLE32(const std::vector<uint8_t> &data, size_t pos) {
    return static_cast<uint32_t>(data[pos]) |
           static_cast<uint32_t>(data[pos + 1]) << 8 |
           static_cast<uint32_t>(data[pos + 2]) << 16 |
           static_cast<uint32_t>(data[pos + 3]) << 24;
}

std::string filenameFromPath(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string stemFromFilename(const std::string &filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) {
        return filename;
    }
    return filename.substr(0, dot);
}

std::string extensionFromFilename(const std::string &filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) {
        return "";
    }
    return filename.substr(dot);
}

std::string joinPath(const std::string &base, const std::string &leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (base.back() == '/') {
        return base + leaf;
    }
    return base + "/" + leaf;
}

bool isDirectory(const std::string &path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool isRegularFile(const std::string &path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

bool isLflFile(const std::string &path) {
    const std::string filename = filenameFromPath(path);
    return toLowerAscii(extensionFromFilename(filename)) == ".lfl";
}

int parseRoomIdFromFilename(const std::string &filename) {
    const std::string stem = stemFromFilename(filename);
    if (stem.empty()) {
        return -1;
    }
    if (!std::all_of(stem.begin(), stem.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return -1;
    }
    try {
        return std::stoi(stem);
    } catch (...) {
        return -1;
    }
}

std::vector<uint8_t> readFileFully(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("Failed to determine file size: " + path);
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(length));
    if (!data.empty()) {
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input) {
            throw std::runtime_error("Failed to read file: " + path);
        }
    }
    return data;
}

std::vector<std::string> listRegularFiles(const std::string &directory) {
    DIR *dir = opendir(directory.c_str());
    if (!dir) {
        throw std::runtime_error("Failed to open directory: " + directory);
    }

    std::vector<std::string> files;
    while (const dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string fullPath = joinPath(directory, name);
        if (isRegularFile(fullPath)) {
            files.push_back(fullPath);
        }
    }
    closedir(dir);
    return files;
}

void ensureDirectoryRecursive(const std::string &path) {
    if (path.empty()) {
        throw std::runtime_error("Output directory path is empty");
    }

    if (isDirectory(path)) {
        return;
    }

    std::string current;
    size_t pos = 0;
    if (!path.empty() && path[0] == '/') {
        current = "/";
        pos = 1;
    }

    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string component = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!component.empty()) {
            if (!current.empty() && current.back() != '/') {
                current += '/';
            }
            current += component;

            if (!isDirectory(current)) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    throw std::runtime_error("Failed to create directory: " + current);
                }
            }
        }

        if (slash == std::string::npos) {
            break;
        }
        pos = slash + 1;
    }
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  indy3_dialogue_extract --game-dir <path> --out <path> [options]\n\n"
        << "Options:\n"
        << "  --split-waits   Split normalized lines on <WAIT> tokens\n"
        << "  --dedupe        Dedupe JSONL lines by normalized text\n"
        << "  --strict        Fail on first parse/format error\n"
        << "  --include-raw   Include raw string bytes as hex in JSONL\n"
        << "  --verbose       Print additional diagnostics\n";
}

Options parseOptions(int argc, char **argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--game-dir") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for --game-dir");
            }
            options.gameDir = argv[++i];
        } else if (arg == "--out") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for --out");
            }
            options.outDir = argv[++i];
        } else if (arg == "--split-waits") {
            options.splitWaits = true;
        } else if (arg == "--dedupe") {
            options.dedupe = true;
        } else if (arg == "--strict") {
            options.strict = true;
        } else if (arg == "--include-raw") {
            options.includeRaw = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.gameDir.empty() || options.outDir.empty()) {
        throw std::runtime_error("--game-dir and --out are required");
    }
    return options;
}

class Extractor {
public:
    explicit Extractor(Options options)
        : _options(std::move(options)) {
    }

    void run() {
        const std::string indexPath = joinPath(_options.gameDir, "00.LFL");
        _scriptIndexByRoomOffset = parseScriptIndex(indexPath);

        std::vector<std::string> lflFiles;
        for (const std::string &path : listRegularFiles(_options.gameDir)) {
            if (!isLflFile(path)) {
                continue;
            }
            lflFiles.push_back(path);
        }

        std::sort(lflFiles.begin(), lflFiles.end(), [](const std::string &a, const std::string &b) {
            return filenameFromPath(a) < filenameFromPath(b);
        });

        for (const std::string &file : lflFiles) {
            const std::string filename = filenameFromPath(file);
            if (toLowerAscii(filename) == "00.lfl") {
                continue;
            }
            collectSourcesFromRoomFile(file);
        }

        scanAllSources();
        if (kEnableMissingDialogueAudit) {
            collectMissingDialogueCandidates();
        }

        ensureDirectoryRecursive(_options.outDir);
        writeJsonl(joinPath(_options.outDir, "dialogues.jsonl"));
        writeUnique(joinPath(_options.outDir, "dialogues_unique.txt"));
        writeStats(joinPath(_options.outDir, "stats.json"));
        if (kEnableMissingDialogueAudit) {
            writeMissingCandidates(joinPath(_options.outDir, "dialogues_missed_candidates.txt"));
        }

        std::cout
            << "Extracted " << _lines.size() << " lines ("
            << _uniqueNormalizedCount << " unique) from "
            << _sources.size() << " script sources.\n";
        std::cout
            << "Rejected candidates: " << _rejectedCandidates
            << ", recovered: " << _recoveredCandidates
            << ", warnings: " << _warnings << "\n";
    }

private:
    static constexpr uint8_t PARAM_1 = 0x80;
    static constexpr uint8_t PARAM_2 = 0x40;

    Options _options;
    size_t _warnings = 0;
    size_t _candidateOpcodes = 0;
    size_t _rejectedCandidates = 0;
    size_t _recoveredCandidates = 0;
    size_t _uniqueNormalizedCount = 0;
    std::unordered_map<uint64_t, int> _scriptIndexByRoomOffset;
    std::vector<ScriptSource> _sources;
    std::vector<ExtractedLine> _lines;
    std::vector<MissingCandidate> _missingCandidates;

    static uint64_t roomOffsetKey(uint8_t room, uint32_t offset) {
        return (static_cast<uint64_t>(room) << 32) | static_cast<uint64_t>(offset);
    }

    [[noreturn]] void fail(const std::string &where, const std::string &message) const {
        throw std::runtime_error(where + ": " + message);
    }

    bool warnOrFail(const std::string &where, const std::string &message) {
        if (_options.strict) {
            fail(where, message);
        }
        ++_warnings;
        std::cerr << "warning: " << where << ": " << message << "\n";
        return false;
    }

    std::unordered_map<uint64_t, int> parseScriptIndex(const std::string &indexPath) {
        std::unordered_map<uint64_t, int> scriptMap;
        const std::vector<uint8_t> data = readFileFully(indexPath);
        const std::string indexName = filenameFromPath(indexPath);

        forEachChunk(data, 0, static_cast<uint32_t>(data.size()), indexName, [&](const ChunkView &chunk) {
            if (chunk.tag != TAG_S0) {
                return;
            }
            if (chunk.dataSize < 2) {
                warnOrFail(indexName, "S0 chunk too small");
                return;
            }

            const uint16_t count = readLE16(data, chunk.dataOffset);
            const uint64_t expected = 2ULL + static_cast<uint64_t>(count) * 5ULL;
            if (expected > chunk.dataSize) {
                warnOrFail(indexName, "S0 chunk truncated");
                return;
            }

            size_t pos = chunk.dataOffset + 2;
            for (uint16_t scriptId = 0; scriptId < count; ++scriptId) {
                const uint8_t roomNo = data[pos];
                const uint32_t roomOffset = readLE32(data, pos + 1);
                scriptMap[roomOffsetKey(roomNo, roomOffset)] = scriptId;
                pos += 5;
            }
        });

        if (_options.verbose) {
            std::cerr << "Loaded " << scriptMap.size() << " script directory entries from 00.LFL\n";
        }

        return scriptMap;
    }

    template <typename Callback>
    void forEachChunk(const std::vector<uint8_t> &data, uint32_t start, uint32_t end,
                      const std::string &context, Callback callback) {
        if (end > data.size() || start > end) {
            warnOrFail(context, "invalid chunk iteration range");
            return;
        }

        uint32_t pos = start;
        while (pos + 6 <= end) {
            const uint32_t size = readLE32(data, pos);
            const uint16_t tag = readLE16(data, pos + 4);

            if (size < 6) {
                warnOrFail(context, "invalid chunk size " + std::to_string(size) + " at offset " + std::to_string(pos));
                return;
            }

            const uint64_t chunkEnd = static_cast<uint64_t>(pos) + static_cast<uint64_t>(size);
            if (chunkEnd > end) {
                warnOrFail(context, "chunk at offset " + std::to_string(pos) + " exceeds parent boundary");
                return;
            }

            ChunkView view;
            view.tag = tag;
            view.size = size;
            view.fileOffset = pos;
            view.dataOffset = pos + 6;
            view.dataSize = size - 6;
            callback(view);

            pos += size;
        }

        if (pos != end) {
            warnOrFail(context, "trailing bytes in chunk range at offset " + std::to_string(pos));
        }
    }

    void collectSourcesFromRoomFile(const std::string &path) {
        const std::string filename = filenameFromPath(path);
        const int roomId = parseRoomIdFromFilename(filename);
        auto buffer = std::make_shared<std::vector<uint8_t>>(readFileFully(path));

        if (_options.verbose) {
            std::cerr << "Parsing " << filename << " (" << buffer->size() << " bytes)\n";
        }

        forEachChunk(*buffer, 0, static_cast<uint32_t>(buffer->size()), filename, [&](const ChunkView &chunk) {
            if (chunk.tag == TAG_SC) {
                int scriptId = -1;
                if (roomId >= 0) {
                    const auto it = _scriptIndexByRoomOffset.find(roomOffsetKey(static_cast<uint8_t>(roomId), chunk.fileOffset));
                    if (it != _scriptIndexByRoomOffset.end()) {
                        scriptId = it->second;
                    }
                }

                ScriptSource src;
                src.kind = SourceKind::GlobalScriptSC;
                src.lflFile = filename;
                src.roomId = roomId;
                src.scriptId = scriptId;
                src.baseOffsetFile = chunk.dataOffset;
                src.len = chunk.dataSize;
                src.buffer = buffer;
                _sources.push_back(std::move(src));
                return;
            }

            if (chunk.tag != TAG_RO) {
                return;
            }

            forEachChunk(*buffer, chunk.dataOffset, chunk.fileOffset + chunk.size, filename + ":RO", [&](const ChunkView &child) {
                if (child.tag == TAG_EN || child.tag == TAG_EX || child.tag == TAG_LS) {
                    if (child.tag == TAG_LS && child.dataSize == 0) {
                        return;
                    }

                    ScriptSource src;
                    src.kind = (child.tag == TAG_EN) ? SourceKind::RoomEntryEN
                            : (child.tag == TAG_EX) ? SourceKind::RoomExitEX
                                                    : SourceKind::LocalScriptLS;
                    src.lflFile = filename;
                    src.roomId = roomId;
                    src.baseOffsetFile = child.dataOffset;
                    src.len = child.dataSize;
                    src.buffer = buffer;
                    _sources.push_back(std::move(src));
                    return;
                }

                if (child.tag != TAG_OC) {
                    return;
                }

                const int objectId = (child.dataSize >= 2) ? static_cast<int>(readLE16(*buffer, child.dataOffset)) : -1;

                ScriptSource ocSource;
                ocSource.kind = SourceKind::ObjectCodeOC;
                ocSource.lflFile = filename;
                ocSource.roomId = roomId;
                ocSource.objectId = objectId;
                ocSource.baseOffsetFile = child.fileOffset;
                ocSource.len = child.size;
                ocSource.buffer = buffer;
                _sources.push_back(std::move(ocSource));

                if (child.size <= 19) {
                    return;
                }

                const uint32_t verbTableStart = child.fileOffset + 19;
                const uint32_t chunkEnd = child.fileOffset + child.size;
                uint32_t pos = verbTableStart;
                std::set<std::pair<uint8_t, uint16_t>> seen;

                while (pos + 2 < chunkEnd) {
                    const uint8_t verbId = (*buffer)[pos];
                    if (verbId == 0) {
                        break;
                    }
                    const uint16_t entryOffset = readLE16(*buffer, pos + 1);
                    pos += 3;

                    if (entryOffset >= child.size) {
                        warnOrFail(filename, "out-of-bounds verb entry offset " + std::to_string(entryOffset)
                            + " in object " + std::to_string(objectId));
                        continue;
                    }

                    if (!seen.insert({verbId, entryOffset}).second) {
                        continue;
                    }

                    ScriptSource verbSource;
                    verbSource.kind = SourceKind::ObjectVerbScript;
                    verbSource.lflFile = filename;
                    verbSource.roomId = roomId;
                    verbSource.objectId = objectId;
                    verbSource.verbId = static_cast<int>(verbId);
                    verbSource.baseOffsetFile = child.fileOffset + entryOffset;
                    verbSource.len = child.size - entryOffset;
                    verbSource.buffer = buffer;
                    _sources.push_back(std::move(verbSource));
                }
            });
        });
    }

    bool consumeByteArg(uint8_t cmd, uint8_t mask, uint32_t &cursor, const ScriptSource &src) {
        const uint32_t argSize = (cmd & mask) ? 2U : 1U;
        if (cursor + argSize > src.len) {
            return false;
        }
        cursor += argSize;
        return true;
    }

    bool consumeWordArg(uint32_t &cursor, const ScriptSource &src) {
        if (cursor + 2 > src.len) {
            return false;
        }
        cursor += 2;
        return true;
    }

    int normalizeActorIdCandidate(int value) {
        if (value < 0) {
            return -1;
        }
        if (value <= 255) {
            return value;
        }
        return -1;
    }

    int resolveVariableByBackwardMoves(const ScriptSource &src, int varId, uint32_t stopOffset, int depth,
                                       std::unordered_set<int> &seenVars) {
        if (varId < 0 || depth > 8) {
            return -1;
        }
        if (!seenVars.insert(varId).second) {
            return -1;
        }

        struct Assignment {
            uint32_t pos = 0;
            bool isDirect = false;
            int directValue = -1;
            int sourceVar = -1;
        };

        std::vector<Assignment> assignments;
        const std::vector<uint8_t> &data = *src.buffer;
        const uint32_t base = src.baseOffsetFile;
        const uint32_t end = std::min(stopOffset, src.len);

        for (uint32_t pos = 0; pos + 4 < end; ++pos) {
            const uint8_t op = data[base + pos];
            if (op != 0x1A && op != 0x9A) {
                continue;
            }

            const int destVar = static_cast<int>(readLE16(data, base + pos + 1));
            if (destVar != varId) {
                continue;
            }

            Assignment a;
            a.pos = pos;
            if (op == 0x1A) {
                a.isDirect = true;
                a.directValue = static_cast<int>(readLE16(data, base + pos + 3));
            } else {
                a.isDirect = false;
                a.sourceVar = static_cast<int>(readLE16(data, base + pos + 3));
            }
            assignments.push_back(a);
        }

        for (auto it = assignments.rbegin(); it != assignments.rend(); ++it) {
            if (it->isDirect) {
                const int actor = normalizeActorIdCandidate(it->directValue);
                if (actor >= 0) {
                    seenVars.erase(varId);
                    return actor;
                }
                continue;
            }

            if (it->sourceVar == varId) {
                continue;
            }

            const int actor = resolveVariableByBackwardMoves(src, it->sourceVar, it->pos, depth + 1, seenVars);
            if (actor >= 0) {
                seenVars.erase(varId);
                return actor;
            }
        }

        seenVars.erase(varId);
        return -1;
    }

    int resolveSpeakerActorLite(const ScriptSource &src, uint32_t opcodeOffset, const ParsedCandidate &parsed) {
        if (parsed.speakerRefKind == SpeakerRefKind::DirectActor) {
            return normalizeActorIdCandidate(parsed.speakerActorId);
        }

        if (parsed.speakerRefKind == SpeakerRefKind::Variable) {
            std::unordered_set<int> seenVars;
            return resolveVariableByBackwardMoves(src, parsed.speakerVarId, opcodeOffset, 0, seenVars);
        }

        if (parsed.speakerRefKind == SpeakerRefKind::Ego) {
            // Indy 3 uses actor 1 as ego in all gameplay contexts.
            return 1;
        }

        return -1;
    }

    int lineQuality(const ExtractedLine &line) {
        int score = 0;
        if (!line.recovered) {
            score += 4;
        }
        if (line.speakerActorId >= 0) {
            score += 8;
        }
        if (line.speakerRefKind == SpeakerRefKind::DirectActor) {
            score += 2;
        } else if (line.speakerRefKind == SpeakerRefKind::Ego) {
            score += 1;
        }
        return score;
    }

    bool scummResStrLen(const ScriptSource &src, uint32_t start, uint32_t &outLen) {
        uint32_t cursor = start;
        const std::vector<uint8_t> &data = *src.buffer;
        const uint32_t base = src.baseOffsetFile;

        while (cursor < src.len) {
            const uint8_t chr = data[base + cursor];
            if (chr == 0) {
                outLen = cursor - start;
                return true;
            }

            ++cursor;
            if (chr == 0xFF) {
                if (cursor >= src.len) {
                    return false;
                }
                const uint8_t code = data[base + cursor];
                ++cursor;
                if (code != 1 && code != 2 && code != 3 && code != 8) {
                    if (cursor + 2 > src.len) {
                        return false;
                    }
                    cursor += 2;
                }
            }
        }

        return false;
    }

    ParsedCandidate parsePrintCandidate(const ScriptSource &src, uint32_t opcodeOffset) {
        ParsedCandidate result;
        const std::vector<uint8_t> &data = *src.buffer;
        const uint32_t base = src.baseOffsetFile;
        const uint8_t opcode = data[src.baseOffsetFile + opcodeOffset];
        result.opcode = opcode;
        result.fromPrintEgo = (opcode == 0xD8);

        uint32_t cursor = opcodeOffset + 1;

        if (opcode == 0x14 || opcode == 0x94) {
            if (opcode & PARAM_1) {
                if (cursor + 2 > src.len) {
                    return result;
                }
                result.speakerRefKind = SpeakerRefKind::Variable;
                result.speakerVarId = static_cast<int>(readLE16(data, base + cursor));
                cursor += 2;
            } else {
                if (cursor + 1 > src.len) {
                    return result;
                }
                result.speakerRefKind = SpeakerRefKind::DirectActor;
                result.speakerActorId = static_cast<int>(data[base + cursor]);
                cursor += 1;
            }
        } else if (opcode == 0xD8) {
            result.speakerRefKind = SpeakerRefKind::Ego;
        }

        while (cursor < src.len) {
            const uint8_t cmd = data[src.baseOffsetFile + cursor];
            ++cursor;

            if (cmd == 0xFF) {
                return result;
            }

            switch (cmd & 0x0F) {
            case 0:
                if (!consumeWordArg(cursor, src) || !consumeWordArg(cursor, src)) {
                    return result;
                }
                break;
            case 1:
                if (!consumeByteArg(cmd, PARAM_1, cursor, src)) {
                    return result;
                }
                break;
            case 2:
                if (!consumeWordArg(cursor, src)) {
                    return result;
                }
                break;
            case 3:
                if (!consumeWordArg(cursor, src) || !consumeWordArg(cursor, src)) {
                    return result;
                }
                break;
            case 4:
                break;
            case 6:
                if (!consumeWordArg(cursor, src)) {
                    return result;
                }
                break;
            case 7:
                break;
            case 8:
                if (!consumeWordArg(cursor, src) || !consumeWordArg(cursor, src)) {
                    return result;
                }
                break;
            case 15: {
                uint32_t msgLen = 0;
                if (!scummResStrLen(src, cursor, msgLen)) {
                    return result;
                }
                result.ok = true;
                result.messageOffsetInSource = cursor;
                result.messageLen = msgLen;
                return result;
            }
            default:
                return result;
            }
        }

        return result;
    }

    std::string messageRawHex(const ScriptSource &src, uint32_t msgOffset, uint32_t msgLen) {
        std::ostringstream out;
        const std::vector<uint8_t> &data = *src.buffer;
        const uint32_t start = src.baseOffsetFile + msgOffset;
        for (uint32_t i = 0; i < msgLen; ++i) {
            out << hexByte(data[start + i]);
        }
        return out.str();
    }

    std::string normalizeMessage(const ScriptSource &src, uint32_t msgOffset, uint32_t msgLen) {
        std::string out;
        const std::vector<uint8_t> &data = *src.buffer;
        uint32_t cursor = src.baseOffsetFile + msgOffset;
        const uint32_t end = cursor + msgLen;

        while (cursor < end) {
            const uint8_t c = data[cursor++];
            if (c == 0xFF && cursor < end) {
                const uint8_t code = data[cursor++];

                if (code == 1 || code == 2 || code == 3 || code == 8) {
                    switch (code) {
                    case 1:
                        out += "<CTRL1>";
                        break;
                    case 2:
                        out += "<CTRL2>";
                        break;
                    case 3:
                        out += "<WAIT>";
                        break;
                    case 8:
                        out += "<CTRL8>";
                        break;
                    default:
                        break;
                    }
                    continue;
                }

                if (cursor + 2 <= end) {
                    const uint16_t arg = static_cast<uint16_t>(data[cursor]) |
                                         static_cast<uint16_t>(data[cursor + 1]) << 8;
                    cursor += 2;

                    switch (code) {
                    case 4:
                        out += "<INT:" + std::to_string(arg) + ">";
                        break;
                    case 5:
                        out += "<VERB:" + std::to_string(arg) + ">";
                        break;
                    case 6:
                        out += "<NAME:" + std::to_string(arg) + ">";
                        break;
                    case 7:
                        out += "<STR:" + std::to_string(arg) + ">";
                        break;
                    default:
                        out += "<CTRL" + std::to_string(code) + ":" + std::to_string(arg) + ">";
                        break;
                    }
                    continue;
                }

                out += "<BROKEN_CTRL:" + std::to_string(code) + ">";
                continue;
            }

            if (c == '\r' || c == '\n' || c == '\t') {
                out.push_back(' ');
            } else if (c >= 32 && c <= 126) {
                out.push_back(static_cast<char>(c));
            } else {
                out += "\\x" + hexByte(c);
            }
        }

        return trim(out);
    }

    bool looksLikeDialogueText(const std::string &text) {
        if (text.size() < 6) {
            return false;
        }
        if (text.find("\\x") != std::string::npos) {
            return false;
        }
        if (text.find('<') != std::string::npos) {
            return false;
        }

        int alpha = 0;
        bool hasSeparator = false;
        for (char c : text) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalpha(uc)) {
                ++alpha;
            }
            if (c == ' ') {
                hasSeparator = true;
            }
        }

        if (alpha < 4 || !hasSeparator) {
            return false;
        }

        const unsigned char first = static_cast<unsigned char>(text[0]);
        if (!std::isupper(first) && text[0] != '"' && text[0] != '\'') {
            return false;
        }

        const char last = text.back();
        return last == '.' || last == '!' || last == '?';
    }

    ParsedCandidate recoverPrintCandidate(const ScriptSource &src, uint32_t opcodeOffset, const ParsedCandidate &seed) {
        ParsedCandidate recovered = seed;
        const std::vector<uint8_t> &data = *src.buffer;
        const uint32_t base = src.baseOffsetFile;

        uint32_t cursor = opcodeOffset + 1;
        if (seed.opcode == 0x14 || seed.opcode == 0x94) {
            const uint32_t actorArgSize = (seed.opcode & PARAM_1) ? 2U : 1U;
            if (cursor + actorArgSize > src.len) {
                return recovered;
            }
            cursor += actorArgSize;
        }

        const uint32_t searchEnd = std::min(src.len, cursor + 96U);
        for (uint32_t pos = cursor; pos < searchEnd; ++pos) {
            const uint8_t cmd = data[base + pos];
            if (cmd == 0xFF) {
                break;
            }

            if (cmd != 0x0F) {
                continue;
            }

            if (pos + 1 >= src.len) {
                break;
            }

            uint32_t msgLen = 0;
            if (!scummResStrLen(src, pos + 1, msgLen)) {
                continue;
            }
            if (msgLen == 0 || msgLen > 600) {
                continue;
            }

            const std::string normalized = normalizeMessage(src, pos + 1, msgLen);
            if (!looksLikeDialogueText(normalized)) {
                continue;
            }

            recovered.ok = true;
            recovered.recovered = true;
            recovered.messageOffsetInSource = pos + 1;
            recovered.messageLen = msgLen;
            return recovered;
        }

        return recovered;
    }

    std::vector<std::string> splitOnWait(const std::string &text) {
        std::vector<std::string> out;
        const std::string token = "<WAIT>";

        size_t start = 0;
        while (start <= text.size()) {
            const size_t pos = text.find(token, start);
            if (pos == std::string::npos) {
                const std::string piece = trim(text.substr(start));
                if (!piece.empty()) {
                    out.push_back(piece);
                }
                break;
            }

            const std::string piece = trim(text.substr(start, pos - start));
            if (!piece.empty()) {
                out.push_back(piece);
            }
            start = pos + token.size();
        }

        if (out.empty() && !text.empty()) {
            out.push_back(trim(text));
        }
        return out;
    }

    std::string canonicalizeForAudit(const std::string &text) {
        std::string out;
        out.reserve(text.size());

        bool inTag = false;
        bool lastSpace = false;
        for (char c : text) {
            if (c == '<') {
                inTag = true;
                continue;
            }
            if (c == '>' && inTag) {
                inTag = false;
                continue;
            }
            if (inTag) {
                continue;
            }

            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isspace(uc)) {
                if (!lastSpace) {
                    out.push_back(' ');
                    lastSpace = true;
                }
                continue;
            }

            out.push_back(static_cast<char>(std::tolower(uc)));
            lastSpace = false;
        }

        return trim(out);
    }

    bool looksLikeMissingDialogueCandidate(const std::string &text) {
        if (text.size() < 6 || text.size() > 220) {
            return false;
        }
        if (!std::isupper(static_cast<unsigned char>(text[0])) && text[0] != '"' && text[0] != '\'') {
            return false;
        }
        if (text.find(' ') == std::string::npos) {
            return false;
        }
        if (text.find("\\x") != std::string::npos || text.find('<') != std::string::npos) {
            return false;
        }

        int alpha = 0;
        bool hasSentencePunct = false;
        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalpha(uc)) {
                ++alpha;
            }
            if (c == '.' || c == '!' || c == '?') {
                hasSentencePunct = true;
            }
        }

        if (alpha < 5) {
            return false;
        }

        return hasSentencePunct;
    }

    void collectMissingDialogueCandidates() {
        std::unordered_set<std::string> extractedCanonical;
        extractedCanonical.reserve(_lines.size() * 2);
        for (const ExtractedLine &line : _lines) {
            extractedCanonical.insert(canonicalizeForAudit(line.textNormalized));
        }

        std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> fileBuffers;
        for (const ScriptSource &src : _sources) {
            fileBuffers[src.lflFile] = src.buffer;
        }

        std::unordered_set<std::string> seenMissingCanonical;
        for (const auto &entry : fileBuffers) {
            const std::string &lflFile = entry.first;
            const std::vector<uint8_t> &data = *entry.second;

            uint32_t i = 0;
            while (i < data.size()) {
                if (data[i] < 32 || data[i] > 126) {
                    ++i;
                    continue;
                }

                uint32_t j = i;
                while (j < data.size() && data[j] >= 32 && data[j] <= 126) {
                    ++j;
                }

                if (j < data.size() && data[j] == 0) {
                    const std::string s(reinterpret_cast<const char *>(&data[i]), j - i);
                    const std::string trimmed = trim(s);
                    if (looksLikeMissingDialogueCandidate(trimmed)) {
                        const std::string key = canonicalizeForAudit(trimmed);
                        if (!key.empty() && extractedCanonical.count(key) == 0 && seenMissingCanonical.insert(key).second) {
                            MissingCandidate m;
                            m.text = trimmed;
                            m.lflFile = lflFile;
                            _missingCandidates.push_back(std::move(m));
                        }
                    }
                }

                i = j + 1;
            }
        }

        std::sort(_missingCandidates.begin(), _missingCandidates.end(), [](const MissingCandidate &a, const MissingCandidate &b) {
            if (a.lflFile != b.lflFile) {
                return a.lflFile < b.lflFile;
            }
            return a.text < b.text;
        });
    }

    void scanAllSources() {
        std::unordered_map<std::string, size_t> dedupeIndex;

        for (const ScriptSource &src : _sources) {
            if (src.len == 0) {
                continue;
            }
            if (src.baseOffsetFile + src.len > src.buffer->size()) {
                warnOrFail(src.lflFile, "invalid source bounds for " + sourceKindToString(src.kind));
                continue;
            }

            const std::vector<uint8_t> &data = *src.buffer;
            for (uint32_t off = 0; off < src.len; ++off) {
                const uint8_t opcode = data[src.baseOffsetFile + off];
                if (opcode != 0x14 && opcode != 0x94 && opcode != 0xD8) {
                    continue;
                }

                ++_candidateOpcodes;
                ParsedCandidate parsed = parsePrintCandidate(src, off);
                if (!parsed.ok && kEnableRecoveredPrintPass) {
                    parsed = recoverPrintCandidate(src, off, parsed);
                    if (parsed.ok) {
                        ++_recoveredCandidates;
                    }
                }
                if (!parsed.ok) {
                    ++_rejectedCandidates;
                    continue;
                }

                const std::string normalized = normalizeMessage(src, parsed.messageOffsetInSource, parsed.messageLen);
                std::vector<std::string> normalizedParts;
                if (_options.splitWaits) {
                    normalizedParts = splitOnWait(normalized);
                } else {
                    normalizedParts.push_back(normalized);
                }

                const std::string rawHex = messageRawHex(src, parsed.messageOffsetInSource, parsed.messageLen);
                for (const std::string &text : normalizedParts) {
                    if (text.empty()) {
                        continue;
                    }

                    ExtractedLine line;
                    line.textRawHex = rawHex;
                    line.textNormalized = text;
                    line.lflFile = src.lflFile;
                    line.sourceKind = src.kind;
                    line.roomId = src.roomId;
                    line.scriptId = src.scriptId;
                    line.objectId = src.objectId;
                    line.verbId = src.verbId;
                    line.opcodeOffsetInSource = off;
                    line.messageOffsetInSource = parsed.messageOffsetInSource;
                    line.opcode = parsed.opcode;
                    line.fromPrintEgo = parsed.fromPrintEgo;
                    line.recovered = parsed.recovered;
                    line.speakerRefKind = parsed.speakerRefKind;
                    line.speakerActorId = resolveSpeakerActorLite(src, off, parsed);
                    line.speakerVarId = parsed.speakerVarId;

                    if (!_options.dedupe) {
                        _lines.push_back(std::move(line));
                        continue;
                    }

                    const auto it = dedupeIndex.find(text);
                    if (it == dedupeIndex.end()) {
                        dedupeIndex[text] = _lines.size();
                        _lines.push_back(std::move(line));
                        continue;
                    }

                    const size_t idx = it->second;
                    if (lineQuality(line) > lineQuality(_lines[idx])) {
                        _lines[idx] = std::move(line);
                    }
                }
            }
        }
    }

    void writeJsonl(const std::string &path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail(path, "failed to open output file");
        }

        for (const ExtractedLine &line : _lines) {
            const bool runtimeRoomKnown = line.sourceKind != SourceKind::GlobalScriptSC;
            const int runtimeRoomId = runtimeRoomKnown ? line.roomId : -1;

            out << "{";
            out << "\"text_normalized\":\"" << jsonEscape(line.textNormalized) << "\"";
            if (_options.includeRaw) {
                out << ",\"text_raw\":\"" << line.textRawHex << "\"";
            }
            out << ",\"source_kind\":\"" << sourceKindToString(line.sourceKind) << "\"";
            out << ",\"lfl_file\":\"" << jsonEscape(line.lflFile) << "\"";
            out << ",\"room_id\":" << runtimeRoomId;
            out << ",\"room_runtime_id\":";
            if (runtimeRoomKnown) {
                out << runtimeRoomId;
            } else {
                out << "null";
            }
            out << ",\"room_source_id\":" << line.roomId;
            out << ",\"script_id\":" << line.scriptId;
            out << ",\"object_id\":" << line.objectId;
            out << ",\"verb_id\":" << line.verbId;
            out << ",\"speaker_kind\":\"" << speakerRefKindToString(line.speakerRefKind) << "\"";
            out << ",\"speaker_actor_id\":" << line.speakerActorId;
            out << ",\"speaker_var_id\":" << line.speakerVarId;
            out << ",\"recovered\":" << (line.recovered ? "true" : "false");
            if (kEmitSpeakerLabel) {
                out << ",\"speaker_label\":\""
                    << jsonEscape(speakerLabelFor(line.speakerRefKind, line.speakerActorId, line.speakerVarId))
                    << "\"";
            }
            out << ",\"opcode\":\"0x" << hexByte(line.opcode) << "\"";
            out << ",\"opcode_offset\":" << line.opcodeOffsetInSource;
            out << ",\"message_offset\":" << line.messageOffsetInSource;
            out << "}\n";
        }
    }

    void writeUnique(const std::string &path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail(path, "failed to open output file");
        }

        std::unordered_set<std::string> seen;
        std::vector<std::string> ordered;
        ordered.reserve(_lines.size());
        for (const ExtractedLine &line : _lines) {
            if (seen.insert(line.textNormalized).second) {
                ordered.push_back(line.textNormalized);
            }
        }
        _uniqueNormalizedCount = ordered.size();

        for (const std::string &text : ordered) {
            out << text << "\n";
        }
    }

    void writeMissingCandidates(const std::string &path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail(path, "failed to open output file");
        }

        out << "# Potentially missed dialogue-like strings (v3 audit)\n";
        out << "# Count: " << _missingCandidates.size() << "\n";
        for (const MissingCandidate &m : _missingCandidates) {
            out << m.lflFile << "\t" << m.text << "\n";
        }
    }

    void writeStats(const std::string &path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail(path, "failed to open output file");
        }

        std::map<std::string, size_t> bySourceKind;
        std::map<std::string, size_t> byFile;
        std::map<std::string, size_t> bySpeakerKind;
        size_t resolvedActorIds = 0;
        for (const ExtractedLine &line : _lines) {
            ++bySourceKind[sourceKindToString(line.sourceKind)];
            ++byFile[line.lflFile];
            ++bySpeakerKind[speakerRefKindToString(line.speakerRefKind)];
            if (line.speakerActorId >= 0) {
                ++resolvedActorIds;
            }
        }

        out << "{\n";
        out << "  \"total_extracted_lines\": " << _lines.size() << ",\n";
        out << "  \"unique_lines\": " << _uniqueNormalizedCount << ",\n";
        out << "  \"sources_scanned\": " << _sources.size() << ",\n";
        out << "  \"candidate_opcodes\": " << _candidateOpcodes << ",\n";
        out << "  \"rejected_candidates\": " << _rejectedCandidates << ",\n";
        out << "  \"recovered_candidates\": " << _recoveredCandidates << ",\n";
        out << "  \"warnings\": " << _warnings << ",\n";
        out << "  \"resolved_speaker_actor_ids\": " << resolvedActorIds << ",\n";
        out << "  \"missing_dialogue_candidates\": " << _missingCandidates.size() << ",\n";

        out << "  \"counts_by_source_kind\": {\n";
        for (auto it = bySourceKind.begin(); it != bySourceKind.end(); ++it) {
            out << "    \"" << jsonEscape(it->first) << "\": " << it->second;
            if (std::next(it) != bySourceKind.end()) {
                out << ",";
            }
            out << "\n";
        }
        out << "  },\n";

        out << "  \"counts_by_speaker_kind\": {\n";
        for (auto it = bySpeakerKind.begin(); it != bySpeakerKind.end(); ++it) {
            out << "    \"" << jsonEscape(it->first) << "\": " << it->second;
            if (std::next(it) != bySpeakerKind.end()) {
                out << ",";
            }
            out << "\n";
        }
        out << "  },\n";

        out << "  \"counts_by_file\": {\n";
        for (auto it = byFile.begin(); it != byFile.end(); ++it) {
            out << "    \"" << jsonEscape(it->first) << "\": " << it->second;
            if (std::next(it) != byFile.end()) {
                out << ",";
            }
            out << "\n";
        }
        out << "  }\n";
        out << "}\n";
    }
};

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseOptions(argc, argv);
        Extractor extractor(options);
        extractor.run();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
