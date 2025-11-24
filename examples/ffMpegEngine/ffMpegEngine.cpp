#include "../../src/FFMpegEngine.h"

#include <iostream>

int main() {
	FFMpegEngine ffMpegEngine;
	ffMpegEngine.addGlobalArg("-hide_banner");
	ffMpegEngine.addInput("input.mp4");
	ffMpegEngine.addInput("watermark.png");

	// prepare VAAPI if desired
	// eng.enableVaapi();
	// eng.vaapiPrepareUpload();

	auto& out = ffMpegEngine.addOutput("out.mp4");
	out.map("0:v").map("0:a");
	out.withVideoCodec("h264_nvenc");
	out.withAudioCodec("aac");

	// add overlay via filter_complex and map it to output
	ffMpegEngine.addFilterComplex("[0:v][1:v] overlay=10:10 [vout]");
	out.map("[vout]");

	// synchronous run with progress parsing
	ffMpegEngine.setDurationMilliSeconds(120000); // 2 minutes -> used to compute percent
	/*
	auto [rc, outtxt] = ffMpegEngine.run(true,
		[](const FFMpegEngine::Progress &p){
			if (p.out_time_ms) {
				std::cout << "progress ms: " << *p.out_time_ms << "\n";
			}
		},
		[](std::string_view l){
			std::cout << "ffmpeg: " << l << "\n";
		}
	);

	std::cout << "exit code: " << rc << "\n";
	if (rc != 0) std::cerr << "ffmpeg error, output:\n" << outtxt << "\n";
	*/
}