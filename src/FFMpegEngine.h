
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <thread>

#include "nlohmann/json.hpp"

#include <fstream>

using namespace std;

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using namespace nlohmann::literals;

class FFMpegEngine {
public:
	struct CallbackData {
		// lower case
		inline static const std::vector<string> errorPatterns = {
			"invalid data found",
			"error while decoding",
			"connection refused",
			"connection timed out",
			"network is unreachable",
			"protocol not found",
			"no such file",
			"broken pipe",
			"unknown encoder",
			"invalid argument",
			"403 forbidden", // url forbidden
			"non-monotonous dts in output stream",
			"404 not found"	// url not found
		};

		CallbackData() = default;
		// Copy assignment operator that ignores ffmpegOutputLogFile
		CallbackData& operator=(const CallbackData& other)
		{
			if (this == &other)
				return *this;

			processedFrames = other.processedFrames;
			framePerSeconds = other.framePerSeconds;
			processedOutputTimestampMilliSecs = other.processedOutputTimestampMilliSecs;
			speed = other.speed;
			dropFrames = other.dropFrames;
			dupFrames = other.dupFrames;
			stream_0_0_q = other.stream_0_0_q;
			stream_1_0_q = other.stream_1_0_q;
			totalSizeKBps = other.totalSizeKBps;
			bitRateKbps = other.bitRateKbps;
			avgBitRateKbps = other.avgBitRateKbps;
			finished = other.finished;
			_errorMessages = other._errorMessages;

			urlForbidden = other.urlForbidden;
			urlNotFound = other.urlNotFound;

			signal = other.signal;

			// ffmpegOutputLogFile non viene copiato perchè non è copiabile (La sua copy-constructor è deleted)

			return *this;
		}

		static constexpr int32_t maxErrorsStored = 50;
		void pushErrorMessage(const string& errorMessage)
		{
			if (_errorMessages.size() >= maxErrorsStored)
				_errorMessages.pop();
			_errorMessages.push(errorMessage);
			if (!urlForbidden && errorMessage.starts_with("403 forbidden"))
				urlForbidden = true;
			if (!urlNotFound && errorMessage.starts_with("404 not found"))
				urlNotFound = true;
		}

		void reset()
		{
			processedFrames = 0;
			framePerSeconds = 0.0;
			processedOutputTimestampMilliSecs = chrono::milliseconds(0);
			speed = 0.0;
			dropFrames = 0;
			dupFrames = 0;
			stream_0_0_q = 0.0;
			stream_1_0_q = 0.0;
			totalSizeKBps = 0;
			bitRateKbps = 0.0;
			avgBitRateKbps = 0.0;

			urlForbidden = false;
			urlNotFound = false;

			signal = nullopt;

			finished = nullopt;

			while (!_errorMessages.empty())
				_errorMessages.pop();
		}

		json toJson()
		{
			if (!finished) // indica che Data non è stato usato
				return nullptr;
			json root;
			root["processedFrames"] = processedFrames;
			root["framePerSeconds"] = framePerSeconds;
			root["processedOutputTimestampMilliSecs"] = processedOutputTimestampMilliSecs.count();
			root["speed"] = speed;
			root["dropFrames"] = dropFrames;
			root["dupFrames"] = dupFrames;
			root["stream_0_0_q"] = stream_0_0_q;
			root["stream_1_0_q"] = stream_1_0_q;
			root["totalSizeKBps"] = totalSizeKBps;
			root["bitRateKbps"] = bitRateKbps;
			root["avgBitRateKbps"] = avgBitRateKbps;
			root["urlForbidden"] = urlForbidden;
			root["urlNotFound"] = urlNotFound;
			root["signal"] = signal ? *signal : -1;
			root["finished"] = *finished;

			json errorMessagesRoot = json::array();
			auto tmp = _errorMessages;   // copia della queue
			while (!tmp.empty()) {
				errorMessagesRoot.push_back(tmp.front());
				tmp.pop();
			}
			root["errorMessages"] = errorMessagesRoot;
			return root;
		}

		ofstream ffmpegOutputLogFile;

		int32_t processedFrames{};
		double framePerSeconds{};
		chrono::milliseconds processedOutputTimestampMilliSecs{};
		double speed{}; // Utile per capire se il server sta performando bene
		int32_t dropFrames{};
		int32_t dupFrames{};
		double stream_0_0_q{};
		double stream_1_0_q{};
		size_t totalSizeKBps{};
		double bitRateKbps{};
		double avgBitRateKbps{};	// calculated by us

		bool urlForbidden{};
		bool urlNotFound{};

		optional<int32_t> signal{};

		// nullopt se Data non è stato utilizzato, false se viene usato ma non è ancora terminato, true se viene usato ed è terminato (progress=end)
		optional<bool> finished = nullopt;

	  private:
		queue<string> _errorMessages;
	};

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

	void reset();

private:
    vector<Input> _inputs;
    vector<Output> _outputs;
    vector<string> _filterComplex;
    vector<string> _globalArgs;
    optional<string> _hwAccel;
    optional<string> _vaapiDevice;

    optional<int64_t> _durationMilliSeconds;
};