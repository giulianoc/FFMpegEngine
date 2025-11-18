
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <atomic>
#include <thread>

using namespace std;

class FFMpegEngine {
public:
    class Input {
    	friend FFMpegEngine;

		string _source;
        vector<string> _args;
		int32_t _durationSeconds = -1;
    public:
    	Input() = default;
    	explicit Input(const string_view& source) : _source(source) {}
		Input& setSource(const string_view& source) { _source = source; return *this; }
		Input& setDurationSeconds(const int32_t durationSeconds) { _durationSeconds = durationSeconds; return *this; }
		Input& addArg(const string_view& parameter);
    	Input& addArgs(const string& parameters);
		void buildArgs(vector<string> &args) const;
		[[nodiscard]] string toSingleLine() const;
	};

    class Output {
    	friend FFMpegEngine;

    	string _path;
        vector<string> _maps;
        vector<string> _videoFilters;
        vector<string> _audioFilters;
        optional<string> _videoCodec;
        optional<string> _audioCodec;
        vector<string> _extraArgs;
    public:
        Output() = default;
    	explicit Output(const string_view& path) : _path(path) {}
		Output& setPath(const string_view& path) { _path = path; return *this; }
        Output& map(string_view m) { _maps.emplace_back(m); return *this; }
        Output& withVideoCodec(string_view c) { _videoCodec = string(c); return *this; }
        Output& withAudioCodec(string_view c) { _audioCodec = string(c); return *this; }
        Output& addVideoFilter(string_view f) { _videoFilters.emplace_back(f); return *this; }
    	size_t videoFilterSize() const { return _videoFilters.size(); }
        Output& addAudioFilter(string_view f) { _audioFilters.emplace_back(f); return *this; }
    	size_t audioFilterSize() const { return _audioFilters.size(); }
        Output& addArg(const string_view& parameter);
     	Output& addArgs(const string& parameters);
    	void buildArgs(vector<string>& args) const;
		[[nodiscard]] string toSingleLine() const;
   };

    FFMpegEngine() = default;

	FFMpegEngine& setUserAgent(const string_view& ua);

    // builder
    FFMpegEngine& addGlobalArg(const string_view &a);
	FFMpegEngine& addGlobalArgs(const string& parameters);
    Input& addInput(string_view source);
    Input& addInput();
    Output& addOutput(string_view path);
    Output& addOutput();
    FFMpegEngine& addFilterComplex(const string_view &fc);

    // convenience inputs
	Input& addUdpInput(const string_view& target, optional<int> listenTimeoutMilliSeconds = {});
    Input& addSrtInput(const string_view &target, optional<int> latencyMilliSeconds = {});
    Input& addRtmpInput(const string_view &target);
    Input& addPipeInput(const string_view &spec);

    // HW accel
    FFMpegEngine& enableNvenc();
    FFMpegEngine& enableVaapi(const string_view &device = "/dev/dri/renderD128");
    FFMpegEngine& enableVideoToolbox();

    // VAAPI convenience: prepare upload and choose codec names (adds filters/args as needed)
    // After calling this, for VAAPI outputs prefer videoCodec "h264_vaapi" or "hevc_vaapi"
    FFMpegEngine& vaapiPrepareUpload();

    // TODO: watermark
    FFMpegEngine& addWatermark(Output& out, string_view overlayLabel, string_view pos = "10:10");

    // duration for percent calculation (ms). If set, progress percent = out_time_ms / durationMilliSeconds
    void setDurationMilliSeconds(int64_t durationMilliSeconds);
	static string toSingleLine(vector<string> &args) ;

	// build command (not shell-quoted). useProgressPipe true adds -progress pipe:1
    [[nodiscard]] string build(bool useProgressPipe = false) const;
    [[nodiscard]] vector<string> buildArgs(bool useProgressPipe = false) const;

	[[nodiscard]] string toPrettyString(int indentSpaces = 2) const;
	[[nodiscard]] string toSingleLine() const;

private:
	optional<string> _userAgent;
    vector<Input> _inputs;
    vector<Output> _outputs;
    vector<string> _filterComplex;
    vector<string> _globalArgs;
    optional<string> _hwAccel;
    optional<string> _vaapiDevice;

    optional<int64_t> _durationMilliSeconds;
};