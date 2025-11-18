
#include "FFMpegEngine.h"

#include "StringUtils.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <ranges>
#include <sstream>

// ---------------- Input methods ----------------

FFMpegEngine::Input& FFMpegEngine::Input::addArg(const string_view& parameter)
{
	if (!StringUtils::trim(parameter).empty())
		_args.emplace_back(StringUtils::trim(parameter));
	return *this;
}

FFMpegEngine::Input& FFMpegEngine::Input::addArgs(const string& parameters)
{
	for (auto&& tok :
		parameters | views::split(' ') | views::filter([](auto &&rng){ return !ranges::empty(rng); }))
		_args.emplace_back(tok.begin(), tok.end());
	return *this;
}

void FFMpegEngine::Input::buildArgs(vector<string>& args) const
{
	for (auto& arg : _args)
		args.emplace_back(arg);
	if (_durationSeconds > 0)
	{
		args.emplace_back("-t");
		args.emplace_back(std::format("{}", _durationSeconds));
	}
	args.emplace_back("-i");
	args.emplace_back(_source);
}

string FFMpegEngine::Input::toSingleLine() const
{
	vector<string> args;
	buildArgs(args);

	return FFMpegEngine::toSingleLine(args);
}


// ---------------- Output methods ----------------

FFMpegEngine::Output& FFMpegEngine::Output::addArg(const string_view& parameter)
{
	if (!StringUtils::trim(parameter).empty())
		_extraArgs.emplace_back(StringUtils::trim(parameter));
	return *this;
}

FFMpegEngine::Output& FFMpegEngine::Output::addArgs(const string& parameters)
{
	for (auto&& tok :
		parameters | views::split(' ') | views::filter([](auto &&rng){ return !ranges::empty(rng); }))
		_extraArgs.emplace_back(tok.begin(), tok.end());
	return *this;
}

void FFMpegEngine::Output::buildArgs(vector<string>& args) const
{
	for (auto& map : _maps)
	{
		args.emplace_back("-map");
		args.emplace_back(map);
	}
	if (_videoCodec)
	{
		args.emplace_back("-c:v");
		args.emplace_back(*_videoCodec);
	}
	if (_audioCodec)
	{
		args.emplace_back("-c:a");
		args.emplace_back(*_audioCodec);
	}
	if (!_videoFilters.empty())
	{
		string vf;
		for (size_t index=0; index < _videoFilters.size(); ++index)
		{
			vf += _videoFilters[index];
			if (index + 1 < _videoFilters.size())
				vf += ",";
		}
		args.emplace_back("-vf");
		args.emplace_back(vf);
	}
	if (!_audioFilters.empty())
	{
		string af;
		for (size_t index = 0; index < _audioFilters.size(); ++index)
		{
			af += _audioFilters[index];
			if (index + 1 < _audioFilters.size())
				af += ",";
		}
		args.emplace_back("-af");
		args.emplace_back(af);
	}
	for (auto& extraArg : _extraArgs)
		args.emplace_back(extraArg);
	args.emplace_back(_path);
}

string FFMpegEngine::Output::toSingleLine() const
{
	vector<string> args;
	buildArgs(args);

	return FFMpegEngine::toSingleLine(args);
}

// ---------------- builder methods ----------------

FFMpegEngine& FFMpegEngine::addGlobalArg(const string_view& arg) {
	if (!StringUtils::trim(arg).empty())
	    _globalArgs.emplace_back(StringUtils::trim(arg));
    return *this;
}

FFMpegEngine& FFMpegEngine::addGlobalArgs(const string& parameters)
{
	for (auto&& tok :
		parameters | views::split(' ') | views::filter([](auto &&rng){ return !ranges::empty(rng); }))
		_globalArgs.emplace_back(tok.begin(), tok.end());
    return *this;
}

FFMpegEngine& FFMpegEngine::setUserAgent(const string_view& ua) {
	if (!StringUtils::trim(ua).empty())
		_userAgent = string(StringUtils::trim(ua));
	return *this;
}

FFMpegEngine::Input& FFMpegEngine::addInput(const string_view source) {
	_inputs.emplace_back(source);
	return _inputs.back();
}

FFMpegEngine::Input& FFMpegEngine::addInput() {
	_inputs.emplace_back();
    return _inputs.back();
}

FFMpegEngine::Output& FFMpegEngine::addOutput(const string_view path) {
	_outputs.emplace_back(path);
	return _outputs.back();
}

FFMpegEngine::Output& FFMpegEngine::addOutput() {
    _outputs.emplace_back();
    return _outputs.back();
}

FFMpegEngine& FFMpegEngine::addFilterComplex(const string_view& fc) {
	if (!StringUtils::trim(fc).empty())
	    _filterComplex.emplace_back(StringUtils::trim(fc));
    return *this;
}

FFMpegEngine::Input& FFMpegEngine::addSrtInput(const string_view& target, optional<int> latencyMilliSeconds) {
    auto& in = addInput(std::format("srt://{}", target));
    if (latencyMilliSeconds)
    	in.addArg(string("-timeout ") + to_string(*latencyMilliSeconds));
    return in;
}

FFMpegEngine::Input& FFMpegEngine::addUdpInput(const string_view& target, optional<int> listenTimeoutMilliSeconds) {
	if (listenTimeoutMilliSeconds)
		return addInput(std::format("udp://{}?timeout=", target, *listenTimeoutMilliSeconds * 1000));
	return addInput(std::format("udp://{}", target));
}

FFMpegEngine::Input& FFMpegEngine::addRtmpInput(const string_view& target) {
    return addInput(std::format("rtmp://{}", target));
}

FFMpegEngine::Input& FFMpegEngine::addPipeInput(const string_view& spec) {
    return addInput(spec);
}

FFMpegEngine& FFMpegEngine::enableNvenc() {
    _hwAccel = "nvenc";
    return *this;
}

FFMpegEngine& FFMpegEngine::enableVaapi(const string_view& device) {
    _hwAccel = "vaapi";
    _vaapiDevice = string(device);
    return *this;
}

FFMpegEngine& FFMpegEngine::enableVideoToolbox() {
    _hwAccel = "videotoolbox";
    return *this;
}

// Prepare VAAPI upload filter and ensure device arg is present
FFMpegEngine& FFMpegEngine::vaapiPrepareUpload() {
    // Add a default hwupload filter for use in filter_complex consumers
    // Caller should add [in] format/ hwupload and map the output to encoder
    if (!_vaapiDevice)
    	_vaapiDevice = "/dev/dri/renderD128";
    // It's user's job to craft proper filter_complex, but we add a helper global arg
    _globalArgs.push_back(std::format("-vaapi_device {}", *_vaapiDevice));
    return *this;
}

FFMpegEngine& FFMpegEngine::addWatermark(Output& out, string_view overlayLabel, string_view pos) {
    out.addVideoFilter(string(overlayLabel) + " overlay=" + string(pos));
    return *this;
}

void FFMpegEngine::setDurationMilliSeconds(int64_t durationMilliSeconds) {
    _durationMilliSeconds = durationMilliSeconds;
}

// ---------------- build args (vector) ----------------

string FFMpegEngine::toSingleLine(vector<string>& args)
{
	ostringstream ffmpegArgumentListStream;

	// if (!ffmpegArgumentList.empty())
	// 	copy(ffmpegArgumentList.begin(), ffmpegArgumentList.end(), ostream_iterator<string>(ffmpegArgumentListStream, " "));

	for (size_t index = 0; index < args.size(); ++index)
	{
		if (index)
			ffmpegArgumentListStream << " ";
		// simple quoting for visualization
		if (args[index].find(' ') != string::npos)
			ffmpegArgumentListStream << "\"" << args[index] << "\"";
		else
			ffmpegArgumentListStream << args[index];
	}

	return ffmpegArgumentListStream.str();
}

vector<string> FFMpegEngine::buildArgs(bool useProgressPipe) const {
    vector<string> args;

	args.emplace_back("ffmpeg");

    for (auto& g : _globalArgs)
    	args.emplace_back(g);

    // inputs
    for (auto& input : _inputs)
    	input.buildArgs(args);

    if (!_filterComplex.empty())
    {
        args.emplace_back("-filter_complex");
        // join
        string fc;
        for (size_t i = 0; i < _filterComplex.size(); ++i)
        {
            fc += _filterComplex[i];
            if (i + 1 < _filterComplex.size())
            	fc += ";";
        }
        args.emplace_back(fc);
    }

    // outputs
    for (auto& output : _outputs)
    	output.buildArgs(args);

    if (useProgressPipe)
    {
    	// TODO
        // args.emplace_back("-nostats"); temporaneamente rimosso per compatibilità con la precedente versione di fork
        args.emplace_back("-progress");
        args.emplace_back("pipe:1");
    }

    return args;
}

string FFMpegEngine::build(bool useProgressPipe) const
{
    auto args = buildArgs(useProgressPipe);

	return toSingleLine(args);
}

// ----------------- formatter per mostrare i comandi -----------------

string FFMpegEngine::toPrettyString(const int indentSpaces) const {
	std::ostringstream oss;
	const std::string indent(indentSpaces, ' ');

	// --- INPUTS ---
	oss << "Inputs:\n";
	for (const auto &inp : _inputs) {
		std::string line;
		for (auto &opt : inp._args)
			line += opt + " ";
		line += "-i " + inp._source;
		oss << indent << line << "\n";
	}
	oss << "\n";

	// --- FILTERS ---
	if (!_filterComplex.empty()) {
		oss << "Filters:\n";
		std::vector<std::string> filterLines;
		for (const auto& f : _filterComplex)
				oss << indent << f << "\n";
		oss << "\n";
	}

	// --- OUTPUT ---
	for (const auto& out : _outputs)
	{
		oss << "Output: " << out._path << "\n";
		oss << indent << "Video codec: " << *out._videoCodec << "\n";
		oss << indent << "Audio codec: " << *out._audioCodec << "\n";
		for (const auto& opt : out._extraArgs)
			oss << indent << opt << "\n";
	}
	oss << "\n";

	return oss.str();
}

/// Formato singola linea, come vero comando ffmpeg
std::string FFMpegEngine::toSingleLine() const {

	return build();
}