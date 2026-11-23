// cue2toc.cpp
// Converts a cue sheet into CU2 text and TOC binary files.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <filesystem>

namespace fs = std::filesystem;

bool compatMode = true;
bool stdoutMode = false;
bool quietMode = false;
bool offsetSupplied = false;
bool offsetModeIsAdd = true;
int formatRevision = 2;
std::string cuesheetName;

void printError(const std::string& message) {
    std::cerr << "Cue2toc: Error while processing "
        << cuesheetName << ": " << message << ".\n";
    std::exit(EXIT_FAILURE);
}

void printWarning(const std::string& message) {
    if (!quietMode) {
        std::cerr << "Cue2toc: Warning while processing "
            << cuesheetName << ": " << message << ".\n";
    }
}

std::string trim(const std::string& text) {
    std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }

    std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

int convertTimecodeToSectors(const std::string& timecode) {
    if (timecode.size() != 8 || timecode[2] != ':' || timecode[5] != ':') {
        throw std::runtime_error("Invalid timecode");
    }

    int minutes = std::stoi(timecode.substr(0, 2));
    int seconds = std::stoi(timecode.substr(3, 2));
    int sectors = std::stoi(timecode.substr(6, 2));

    if (seconds > 59 || sectors > 74) {
        throw std::runtime_error("Invalid timecode");
    }

    return minutes * 60 * 75 + seconds * 75 + sectors;
}

std::string convertSectorsToTimecode(int sectors) {
    sectors = std::max(0, sectors);

    int totalSeconds = sectors / 75;
    int moduloSectors = sectors % 75;
    int totalMinutes = totalSeconds / 60;
    int moduloSeconds = totalSeconds % 60;

    std::ostringstream result;
    result << (totalMinutes < 10 ? "0" : "") << totalMinutes << ":"
        << (moduloSeconds < 10 ? "0" : "") << moduloSeconds << ":"
        << (moduloSectors < 10 ? "0" : "") << moduloSectors;
    return result.str();
}

std::string convertSectorsToAlternativeTimecode(int sectors) {
    sectors = std::max(0, sectors);

    int totalSeconds = sectors / 75;
    int moduloSectors = sectors % 75;
    int totalMinutes = totalSeconds / 60;
    int moduloSeconds = totalSeconds % 60;

    if (moduloSectors == 0) {
        moduloSectors = 75;
        if (moduloSeconds != 0) {
            --moduloSeconds;
        }
        else {
            moduloSeconds = 59;
            --totalMinutes;
        }
    }

    totalMinutes = std::max(0, totalMinutes);

    std::ostringstream result;
    result << (totalMinutes < 10 ? "0" : "") << totalMinutes << ":"
        << (moduloSeconds < 10 ? "0" : "") << moduloSeconds << ":"
        << (moduloSectors < 10 ? "0" : "") << moduloSectors;
    return result.str();
}

std::string timecodeAddition(const std::string& timecode,
    const std::string& offset) {
    int result = convertTimecodeToSectors(timecode)
        + convertTimecodeToSectors(offset);
    result = std::min(result, 449999);
    return convertSectorsToTimecode(result);
}

std::string timecodeSubtraction(const std::string& timecode,
    const std::string& offset) {
    int result = convertTimecodeToSectors(timecode)
        - convertTimecodeToSectors(offset);
    result = std::max(result, 0);
    return convertSectorsToTimecode(result);
}

int convertBytesToSectors(std::uint64_t filesize) {
    if (filesize % 2352 == 0) {
        return static_cast<int>(filesize / 2352);
    }

    printError("The filesize of the binary file or --size parameter indicates "
        "that this is not a valid image in MODE2/2352");
    return 0;
}

std::vector<std::string> readCueSheet(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        printError("Could not open " + path);
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return lines;
}

bool lineMatches(const std::string& line, const std::regex& regex) {
    return std::regex_search(line, regex);
}

uint8_t bcd(int value) {
    value = std::max(0, value);
    int tens = (value / 10) % 10;
    int ones = value % 10;
    return static_cast<uint8_t>(ones + 16 * tens);
}

struct Options {
    bool compatibilityExplicit = false;
    bool noCompatibilityExplicit = false;
    bool formatSupplied = false;
    bool sizeSupplied = false;
    bool nameSupplied = false;
    bool offsetSupplied = false;
    bool offsetSelectSupplied = false;

    std::uint64_t size = 0;
    std::string outputName;
    std::string offset;
    std::string offsetSelect;
};

void printHelp(const char* programName) {
    std::cout
        << "Usage: " << programName
        << " [options] cuesheet\n\n"
        << "Options:\n"
        << "  -c, --compat             Enable compatibility mode\n"
        << "  -nc, --nocompat          Disable compatibility mode\n"
        << "  -f, --format 1|2         Set CU2 format revision (default: 2)\n"
        << "  -s, --size BYTES         Specify binary image size manually\n"
        << "  -n, --name FILE          Specify CU2 output filename\n"
        << "  -o, --offset MM:SS:ss    Apply a [+/-]time offset\n"
        << "  -os, --offset-select LETTERS\n"
        << "                           Apply offsets to D, A, P, E, and/or S\n"
        << "  -1, --stdout             Write CU2 text to stdout\n"
        << "  -q, --quiet              Suppress warning messages\n";
}

Options parseArguments(int argc, char* argv[]) {
    Options options;

    if (argc < 2) {
        printHelp(argv[0]);
        std::exit(EXIT_SUCCESS);
    }

    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            printHelp(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        else if (argument == "-c" || argument == "--compat") {
            options.compatibilityExplicit = true;
        }
        else if (argument == "-nc" || argument == "--nocompat") {
            options.noCompatibilityExplicit = true;
        }
        else if (argument == "-1" || argument == "--stdout") {
            stdoutMode = true;
        }
        else if (argument == "-q" || argument == "--quiet") {
            quietMode = true;
        }
        else if (argument == "-f" || argument == "--format") {
            if (++i >= argc) {
                printError("Missing value for --format");
            }
            options.formatSupplied = true;
            formatRevision = std::stoi(argv[i]);
        }
        else if (argument == "-s" || argument == "--size") {
            if (++i >= argc) {
                printError("Missing value for --size");
            }
            options.sizeSupplied = true;
            options.size = std::stoull(argv[i]);
        }
        else if (argument == "-n" || argument == "--name") {
            if (++i >= argc) {
                printError("Missing value for --name");
            }
            options.nameSupplied = true;
            options.outputName = argv[i];
        }
        else if (argument == "-o" || argument == "--offset") {
            if (++i >= argc) {
                printError("Missing value for --offset");
            }
            options.offsetSupplied = true;
            options.offset = argv[i];
        }
        else if (argument == "-os" || argument == "--offset-select") {
            if (++i >= argc) {
                printError("Missing value for --offset-select");
            }
            options.offsetSelectSupplied = true;
            options.offsetSelect = argv[i];
        }
        else if (!argument.empty() && argument[0] == '-') {
            printError("Unknown option: " + argument);
        }
        else {
            cuesheetName = argument;
        }
    }

    if (cuesheetName.empty()) {
        printError("No cue sheet specified");
    }

    return options;
}

int main(int argc, char* argv[]) {
    try {
        Options options = parseArguments(argc, argv);

        if (options.compatibilityExplicit && options.noCompatibilityExplicit) {
            printError("Can not enable and disable compatibility mode at the same time");
        }

        if (options.noCompatibilityExplicit) {
            compatMode = false;
        }
        else {
            compatMode = true;
        }

        if (options.formatSupplied) {
            if (formatRevision != 1 && formatRevision != 2) {
                printError("CU2 format revision must be either 1 or 2");
            }
        }

        std::string offsetTimecode;
        if (options.offsetSupplied) {
            std::regex offsetRegex(
                "^([+-]?)([0-9]{2}:[0-5][0-9]:[0-7][0-9])$");
            std::smatch match;
            if (!std::regex_match(options.offset, match, offsetRegex)) {
                printError("Supplied offset is invalid");
            }

            offsetSupplied = true;
            offsetModeIsAdd =
                match[1].str().empty() || match[1].str() == "+";
            offsetTimecode = match[2].str();
        }

        bool offsetData1 = false;
        bool offsetAudio = true;
        bool offsetPregaps = (formatRevision == 2);
        bool offsetEnd = true;
        bool offsetSize = false;

        if (options.offsetSelectSupplied) {
            if (!offsetSupplied) {
                printError("Offset selection supplied but no offset");
            }

            std::string lower = toLower(options.offsetSelect);
            if (lower.size() > 5 ||
                lower.find_first_not_of("dap es") != std::string::npos ||
                lower.find_first_not_of("dapes") == std::string::npos) {
                printError("Badly formatted offset selection string");
            }

            offsetData1 = lower.find('d') != std::string::npos;
            offsetAudio = lower.find('a') != std::string::npos;
            offsetPregaps = lower.find('p') != std::string::npos;
            offsetEnd = lower.find('e') != std::string::npos;
            offsetSize = lower.find('s') != std::string::npos;

            if (offsetPregaps && formatRevision == 1) {
                printWarning("An offset was to be applied to pregaps, but "
                    "format 1 does not support pregaps");
            }
        }

        std::vector<std::string> lines = readCueSheet(cuesheetName);

        bool modeValid = false;
        std::regex modeRegex(".*[Mm][Oo][Dd][Ee]2/2352.*");
        for (const std::string& line : lines) {
            if (std::regex_match(line, modeRegex)) {
                modeValid = true;
                break;
            }
        }
        if (!modeValid) {
            printError("Cue sheet indicates this image is not in MODE2/2352");
        }

        int fileCount = 0;
        std::regex fileRegex("^[ \\t]*[Ff][Ii][Ll][Ee].*");
        for (const std::string& line : lines) {
            if (std::regex_search(line, fileRegex)) {
                ++fileCount;
            }
        }
        if (fileCount != 1) {
            printError("The cue sheet is invalid or is part of an image with "
                "multiple binary files");
        }

        std::string binaryFile;
        std::regex binaryRegex(
            "^[ \\t]*[Ff][Ii][Ll][Ee].*[Bb][Ii][Nn][Aa][Rr][Yy].*");
        for (const std::string& line : lines) {
            if (std::regex_search(line, binaryRegex)) {
                std::regex nameRegex("\"([^\"]+)\"");
                std::smatch match;
                if (std::regex_search(line, match, nameRegex)) {
                    binaryFile = match[1].str();
                }
                else {
                    std::istringstream stream(line);
                    std::string fileWord;
                    stream >> fileWord >> binaryFile;
                    if (!binaryFile.empty() &&
                        binaryFile.front() == '\"') {
                        binaryFile = trim(binaryFile.substr(
                            1, binaryFile.find_last_of('\"') - 1));
                    }
                }
                break;
            }
        }

        if (binaryFile.empty()) {
            printError("Could not find binary file");
        }

        int trackCount = 0;
        std::regex trackRegex(
            "^[ \\t]*[Tt][Rr][Aa][Cc][Kk][ \\t]+[0-9]+.*");
        for (const std::string& line : lines) {
            if (std::regex_search(line, trackRegex)) {
                ++trackCount;
            }
        }

        std::ostringstream output;
        output << "ntracks " << trackCount << "\r\n";

        int sectors;
        if (options.sizeSupplied) {
            sectors = convertBytesToSectors(options.size);
        }
        else {
            if (!fs::exists(binaryFile)) {
                printError("Cue sheet refers to a binary file, " +
                    binaryFile +
                    ", that could not be found");
            }

            sectors = convertBytesToSectors(
                static_cast<std::uint64_t>(fs::file_size(binaryFile)));
        }

        std::string size = convertSectorsToTimecode(sectors);
        if (compatMode && formatRevision == 1) {
            size = timecodeAddition(size, "00:02:00");
        }
        if (offsetSupplied && offsetSize) {
            size = offsetModeIsAdd
                ? timecodeAddition(size, offsetTimecode)
                : timecodeSubtraction(size, offsetTimecode);
        }
        output << "size      " << size << "\r\n";

        std::string data1 = "00:00:00";
        if (compatMode) {
            data1 = timecodeAddition(data1, "00:02:00");
        }
        if (offsetSupplied && offsetData1) {
            data1 = offsetModeIsAdd
                ? timecodeAddition(data1, offsetTimecode)
                : timecodeSubtraction(data1, offsetTimecode);
        }
        output << "data1     " << data1 << "\r\n";

        bool pregapCommandUsed = false;

        for (int track = 2; track <= trackCount; ++track) {
            int current = -1;
            std::string trackPattern =
                ".*[Tt][Rr][Aa][Cc][Kk][ \\t]+0?" +
                std::to_string(track) + "[ \\t]+.*";

            std::regex trackPatternRegex(trackPattern,
                std::regex::ECMAScript);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (std::regex_search(lines[i], trackPatternRegex)) {
                    current = static_cast<int>(i);
                    break;
                }
            }

            if (current < 0) {
                printError("Could not find track " +
                    std::to_string(track));
            }

            if (current + 1 >= static_cast<int>(lines.size())) {
                printError("Could not find position for track " +
                    std::to_string(track));
            }

            std::regex index00Regex(".*[Ii][Nn][Dd][Ee][Xx][ \\t]+0?0.*");
            std::regex index01Regex(".*[Ii][Nn][Dd][Ee][Xx][ \\t]+0?1.*");
            std::regex pregapRegex(".*[Pp][Rr][Ee][Gg][Aa][Pp].*");

            std::string pregapPosition;

            if (std::regex_search(lines[current + 1], index00Regex) &&
                formatRevision == 2) {
                std::regex timeRegex("([0-9]{2}:[0-5][0-9]:[0-7][0-9])");
                std::smatch match;
                if (!std::regex_search(lines[current + 1],
                    match, timeRegex)) {
                    printError("Could not read pregap position");
                }

                pregapPosition = match[1].str();
                if (compatMode) {
                    pregapPosition =
                        convertSectorsToAlternativeTimecode(
                            convertTimecodeToSectors(
                                timecodeAddition(pregapPosition,
                                    "00:02:00")));
                }
                if (offsetSupplied && offsetPregaps) {
                    pregapPosition =
                        offsetModeIsAdd
                        ? timecodeAddition(pregapPosition,
                            offsetTimecode)
                        : timecodeSubtraction(pregapPosition,
                            offsetTimecode);
                }

                output << "pregap" << (track < 10 ? "0" : "")
                    << track << "  " << pregapPosition << "\r\n";
            }
            else if (std::regex_search(lines[current + 1], pregapRegex) &&
                formatRevision == 2) {
                if (!pregapCommandUsed) {
                    printWarning(
                        "The PREGAP command is used for track " +
                        std::to_string(track) +
                        "; it is ignored and a zero-length pregap is used");
                    pregapCommandUsed = true;
                }
                else {
                    printWarning("The PREGAP command is also used for track " +
                        std::to_string(track));
                }

                if (current + 2 < static_cast<int>(lines.size()) &&
                    std::regex_search(lines[current + 2], index01Regex)) {
                    std::regex timeRegex(
                        "([0-9]{2}:[0-5][0-9]:[0-7][0-9])");
                    std::smatch match;
                    if (std::regex_search(lines[current + 2],
                        match, timeRegex)) {
                        pregapPosition = match[1].str();
                    }
                }

                if (pregapPosition.empty()) {
                    pregapPosition = "00:00:00";
                }

                if (compatMode) {
                    pregapPosition =
                        convertSectorsToAlternativeTimecode(
                            convertTimecodeToSectors(
                                timecodeAddition(pregapPosition,
                                    "00:02:00")));
                }
                if (offsetSupplied && offsetPregaps) {
                    pregapPosition =
                        offsetModeIsAdd
                        ? timecodeAddition(pregapPosition,
                            offsetTimecode)
                        : timecodeSubtraction(pregapPosition,
                            offsetTimecode);
                }

                output << "pregap" << (track < 10 ? "0" : "")
                    << track << "  " << pregapPosition << "\r\n";
            }
            else if (formatRevision == 2) {
                printError("Could not find pregap position (index 00) "
                    "for track " +
                    std::to_string(track));
            }

            std::string trackPosition;
            if (std::regex_search(lines[current + 1], index01Regex)) {
                std::regex timeRegex(
                    "([0-9]{2}:[0-5][0-9]:[0-7][0-9])");
                std::smatch match;
                if (std::regex_search(lines[current + 1],
                    match, timeRegex)) {
                    trackPosition = match[1].str();
                }
            }
            else if (current + 2 < static_cast<int>(lines.size()) &&
                std::regex_search(lines[current + 2], index01Regex)) {
                std::regex timeRegex(
                    "([0-9]{2}:[0-5][0-9]:[0-7][0-9])");
                std::smatch match;
                if (std::regex_search(lines[current + 2],
                    match, timeRegex)) {
                    trackPosition = match[1].str();
                }
            }

            if (trackPosition.empty()) {
                printError("Could not find starting position (index 01) "
                    "for track " +
                    std::to_string(track));
            }

            if (compatMode) {
                trackPosition =
                    convertSectorsToAlternativeTimecode(
                        convertTimecodeToSectors(
                            timecodeAddition(trackPosition, "00:02:00")));
            }
            if (offsetSupplied && offsetAudio) {
                trackPosition =
                    offsetModeIsAdd
                    ? timecodeAddition(trackPosition, offsetTimecode)
                    : timecodeSubtraction(trackPosition, offsetTimecode);
            }

            output << "track" << (track < 10 ? "0" : "")
                << track << "   " << trackPosition << "\r\n";
        }

        std::string trackEnd;
        if (compatMode && formatRevision == 1) {
            trackEnd =
                convertSectorsToAlternativeTimecode(
                    convertTimecodeToSectors(
                        timecodeAddition(size, "00:04:00")));
        }
        else if (compatMode && formatRevision == 2) {
            trackEnd =
                convertSectorsToAlternativeTimecode(
                    convertTimecodeToSectors(
                        timecodeAddition(size, "00:02:00")));
        }
        else {
            trackEnd = size;
        }

        if (offsetSupplied && offsetEnd) {
            trackEnd =
                offsetModeIsAdd
                ? timecodeAddition(trackEnd, offsetTimecode)
                : timecodeSubtraction(trackEnd, offsetTimecode);
        }

        output << "\r\ntrk end   " << trackEnd;
        if (!compatMode && !stdoutMode) {
            output << "\r\n";
        }

        std::string outputText = output.str();

        if (stdoutMode) {
            if (compatMode) {
                printWarning("Piping stdout to a file will add a proper "
                    "linebreak to the last line");
            }
            std::cout << outputText << '\n';
            return EXIT_SUCCESS;
        }

        fs::path cu2Path =
            options.nameSupplied
            ? fs::path(options.outputName)
            : fs::path(binaryFile).replace_extension(".cu2");

        std::ofstream cu2File(cu2Path, std::ios::binary);
        if (!cu2File) {
            printError("Could not write to " + cu2Path.string());
        }
        cu2File.write(outputText.data(),
            static_cast<std::streamsize>(outputText.size()));
        cu2File.close();

        // Create the binary TOC file.
        std::vector<uint8_t> toc = {
            0x41, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00,
            0x01, 0x00, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        toc[17] = bcd(trackCount);
        toc[27] = bcd(std::stoi(trackEnd.substr(0, 2)));
        toc[28] = bcd(std::stoi(trackEnd.substr(3, 2)));
        toc[29] = bcd(std::stoi(trackEnd.substr(6, 2)));

        int track = 1;
        std::istringstream cu2Stream(outputText);
        std::string cu2Line;

        while (std::getline(cu2Stream, cu2Line)) {
            if (!cu2Line.empty() && cu2Line.back() == '\r') {
                cu2Line.pop_back();
            }

            if (cu2Line.rfind("pregap", 0) == 0) {
                std::string msf = cu2Line.substr(10);
                if (toc.size() < 10) {
                    toc.resize(10);
                }

                // Pregaps are appended in the same track-entry format.
                std::vector<uint8_t> buffer(10, 0);
                buffer[3] = bcd(std::stoi(msf.substr(0, 2)));
                buffer[4] = bcd(std::stoi(msf.substr(3, 2)));
                buffer[5] = bcd(std::stoi(msf.substr(6, 2)));
                toc.insert(toc.end(), buffer.begin(), buffer.end());
                continue;
            }

            if (cu2Line.rfind("track", 0) == 0 ||
                cu2Line.rfind("data", 0) == 0) {
                std::string msf = cu2Line.substr(10);

                std::vector<uint8_t> buffer(10, 0);
                buffer[0] = (track == 1) ? 0x41 : 0x01;
                buffer[2] = bcd(track);
                buffer[7] = bcd(std::stoi(msf.substr(0, 2)));
                buffer[8] = bcd(std::stoi(msf.substr(3, 2)));
                buffer[9] = bcd(std::stoi(msf.substr(6, 2)));

                ++track;
                toc.insert(toc.end(), buffer.begin(), buffer.end());
            }
        }

        fs::path tocPath = fs::path(binaryFile).replace_extension(".TOC");

        std::ofstream tocFile(tocPath, std::ios::binary);
        if (!tocFile) {
            printError("Could not write TOC file");
        }

        tocFile.write(reinterpret_cast<const char*>(toc.data()),
            static_cast<std::streamsize>(toc.size()));
        tocFile.close();

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception) {
        std::cerr << "Cue2toc: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}