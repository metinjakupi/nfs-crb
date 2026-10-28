// Decode private D3D9 programs, emit the renderer's MSL and compile it using
// the real Metal compiler. No window, profile, audio device or game is opened.
// Success proves instruction translation/compilation, not live bindings,
// equivalent pixels, supported effect passes or successful game integration.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../../../dx/d3d9_msl.h"
#include "../../../../dx/d3d9_shader.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

static NSString *ns(const std::string &value) {
    return [NSString stringWithUTF8String:value.c_str()] ?: @"invalid UTF-8";
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "usage: d3d9_shader_corpus PROGRAM_DIRECTORY OUTPUT_DIRECTORY\n");
            return 2;
        }
        namespace fs = std::filesystem;
        const fs::path input(argv[1]), output(argv[2]);
        std::error_code error;
        if (!fs::is_directory(input, error) || !fs::create_directory(output, error)) {
            fprintf(stderr, "input must exist; output must be a new directory: %s\n",
                    error.message().c_str());
            return 2;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "no Metal device; corpus was not tested\n");
            return 2;
        }
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(input)) {
            const auto ext = entry.path().extension();
            if (entry.is_regular_file() && (ext == ".vsbin" || ext == ".psbin"))
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        NSMutableArray *results = [NSMutableArray array];
        unsigned failures = 0;
        for (const fs::path &path : files) {
            @autoreleasepool {
                std::ifstream file(path, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
                const auto &program = d9sh::program_for(bytes);
                std::string source, why = program.why;
                bool generated =
                    program.ok && (program.pixel ? d9msl::pixel_source(program, {}, &source, &why)
                                                 : d9msl::vertex_source(program, &source, &why));
                bool compiled = false;
                if (generated) {
                    std::ofstream dump(output / (path.stem().string() + ".metal"));
                    dump << source;
                    if (!dump) {
                        fprintf(stderr, "cannot save generated source\n");
                        return 2;
                    }
                    NSError *compile_error = nil;
                    MTLCompileOptions *options = [MTLCompileOptions new];
                    // Match the production D3D9 backend's floating-point mode.
                    if (@available(macOS 15.0, *))
                        options.mathMode = MTLMathModeSafe;
                    id<MTLLibrary> library = [device newLibraryWithSource:ns(source)
                                                                  options:options
                                                                    error:&compile_error];
                    id<MTLFunction> function =
                        [library newFunctionWithName:program.pixel ? @"ps_main" : @"vs_main"];
                    compiled = function != nil;
                    if (!compiled)
                        why = compile_error.localizedDescription.UTF8String ?: "entry point absent";
                }
                failures += !compiled;
                [results addObject:@{
                    @"file" : ns(path.filename().string()),
                    @"decoded" : @(program.ok),
                    @"generated" : @(generated),
                    @"compiled" : @(compiled),
                    @"major" : @(program.major),
                    @"minor" : @(program.minor),
                    @"pixel" : @(program.pixel),
                    @"instructions" : @(program.code.size()),
                    @"relative_constants" : @(program.relative),
                    @"reason" : ns(why)
                }];
                if (!compiled)
                    fprintf(stderr, "%s: %s\n", path.filename().c_str(), why.c_str());
            }
        }
        NSDictionary *report = @{
            @"device" : device.name,
            @"programs" : @(files.size()),
            @"failures" : @(failures),
            @"variant" : @"default; no external sampler overrides",
            @"scope" : @"Compile only, not rendered pixels or live game bindings",
            @"results" : results
        };
        NSData *json = [NSJSONSerialization dataWithJSONObject:report
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:nil];
        if (![json writeToFile:ns((output / "report.json").string()) atomically:YES])
            return 2;
        printf("shader corpus: %zu programs, %u failures\n", files.size(), failures);
        return files.empty() ? 2 : failures ? 1 : 0;
    }
}
