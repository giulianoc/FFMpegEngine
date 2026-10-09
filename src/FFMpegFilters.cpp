
#include "FFMpegFilters.h"
#include "JsonPath.h"
#include "StringUtils.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <utility>

using namespace std;

FFMpegFilters::FFMpegFilters(std::string  ffmpegTempDir, std::string  ffmpegTtfFontDir, int64_t ingestionJobKey, int64_t encodingJobKey, int outputIndex)
	: _ffmpegTempDir(std::move(ffmpegTempDir)), _ffmpegTtfFontDir(std::move(ffmpegTtfFontDir)), _ingestionJobKey(ingestionJobKey), _encodingJobKey(encodingJobKey),
	  _outputIndex(outputIndex)
{
}

FFMpegFilters::~FFMpegFilters() = default;

std::tuple<std::string, std::string, std::string>
FFMpegFilters::addFilters(
	nlohmann::json filtersRoot, const std::string& ffmpegVideoResolutionParameter, const std::string& ffmpegDrawTextFilter,
	std::optional<int32_t> inputDurationInSeconds) const
{
	std::string videoFilters = addVideoFilters(filtersRoot, ffmpegVideoResolutionParameter, ffmpegDrawTextFilter, inputDurationInSeconds);
	std::string audioFilters = addAudioFilters(filtersRoot, inputDurationInSeconds);
	std::string complexFilters;

	if (filtersRoot != nullptr)
	{
		if (JSONUtils::isPresent(filtersRoot, "complex"))
		{
			for (int filterIndex = 0; filterIndex < filtersRoot["complex"].size(); filterIndex++)
			{
				nlohmann::json filterRoot = filtersRoot["complex"][filterIndex];

				std::string filter = getFilter(filterRoot, inputDurationInSeconds);
				if (!complexFilters.empty())
					complexFilters += ",";
				complexFilters += filter;
			}
		}
	}

	// Simple and complex filtering cannot be used together for the same stream
	if (!complexFilters.empty())
	{
		if (!audioFilters.empty())
			complexFilters = std::format("{},{}", audioFilters, complexFilters);
		if (!videoFilters.empty())
			complexFilters = std::format("{},{}", videoFilters, complexFilters);
		videoFilters = "";
		audioFilters = "";
	}

	return make_tuple(videoFilters, audioFilters, complexFilters);
}

std::string FFMpegFilters::addVideoFilters(
	nlohmann::json filtersRoot, const std::string& ffmpegVideoResolutionParameter, const std::string& ffmpegDrawTextFilter,
	std::optional<int32_t> inputDurationInSeconds
) const
{
	std::string videoFilters;

	if (!ffmpegVideoResolutionParameter.empty())
	{
		if (!videoFilters.empty())
			videoFilters += ',';
		videoFilters += ffmpegVideoResolutionParameter;
	}
	if (!ffmpegDrawTextFilter.empty())
	{
		if (!videoFilters.empty())
			videoFilters += ',';
		videoFilters += ffmpegDrawTextFilter;
	}

	if (filtersRoot != nullptr)
	{
		if (JSONUtils::isPresent(filtersRoot, "video"))
		{
			for (const auto& filterRoot : filtersRoot["video"])
			{
				std::string filter = getFilter(filterRoot, inputDurationInSeconds);
				if (!videoFilters.empty())
					videoFilters += ',';
				videoFilters += filter;
			}
		}
	}

	return videoFilters;
}

std::string FFMpegFilters::addAudioFilters(const nlohmann::json& filtersRoot, std::optional<int32_t> inputDurationInSeconds) const
{
	std::string audioFilters;

	if (filtersRoot != nullptr)
	{
		if (JSONUtils::isPresent(filtersRoot, "audio"))
		{
			for (const auto& filterRoot : filtersRoot["audio"])
			{
				const std::string filter = getFilter(filterRoot, inputDurationInSeconds);
				if (!audioFilters.empty())
					audioFilters += ',';
				audioFilters += filter;
			}
		}
	}

	return audioFilters;
}

std::string FFMpegFilters::getFilter(const nlohmann::json& filterRoot, std::optional<int32_t> inputDurationInSeconds) const
{
	std::string filter;

	if (!JSONUtils::isPresent(filterRoot, "type"))
	{
		std::string errorMessage = "filterRoot->type field does not exist";
		LOG_ERROR(errorMessage);

		throw std::runtime_error(errorMessage);
	}
	// std::string type = JSONUtils::as<string>(filterRoot, "type");
	auto type = JsonPath(&filterRoot)["type"].as<std::string>("");

	switch (hash_case(type))
	{
		case "ametadata"_case:
		{
			filter = ("ametadata=mode=print");

			break;
		}
		case "aresample"_case:
		{
			auto async = JsonPath(&filterRoot)["async"].as<int32_t>(1);
			auto first_pts = JsonPath(&filterRoot)["first_pts"].as<int32_t>(0);
			filter = std::format("aresample=async={}:first_pts={}", async, first_pts);

			break;
		}
		case "ashowinfo"_case:
		{
			filter = ("ashowinfo");

			break;
		}
		case "blackdetect"_case:
		{
			// Set the minimum detected black duration expressed in seconds. It must be a non-negative floating point number.
			// Default value is 2.0.
			auto black_min_duration = JsonPath(&filterRoot)["black_min_duration"].as<double>(2.0);

			// Set the threshold for considering a pixel "black".
			// The threshold expresses the maximum pixel luma value for which a pixel is considered "black".
			// The provided value is scaled according to the following equation:
			// absolute_threshold = luma_minimum_value + pixel_black_th * luma_range_size
			// luma_range_size and luma_minimum_value depend on the input video format, the range is [0-255]
			// for YUV full-range formats and [16-235] for YUV non full-range formats.
			// Default value is 0.10.
			auto pixel_black_th = JsonPath(&filterRoot)["pixel_black_th"].as<double>(0.10);

			// blackdetect logga "black_start:... black_end:... black_duration:..." solo alla fine del nero
			// (e solo se black_duration >= d). Se quindi il nero non finisce non avremmo alcun evento.
			// Per questo motivo abbiamo bisogno quindi anche del filtro metadata che ci dice quando un nero inizia/finisce (anche se è un flash
			// più corto di d), stampiamo i metadata lavfi.black_start/lavfi.black_end.
			// ffmpeg logga:
			//	[Parsed_metadata_1 @ 0x...] frame:312  pts:12480   pts_time:12.48
			//  viene generato dal filtro metadata per ogni frame che contiene la chiave cercata.
			//  Compare quindi sia prima di lavfi.black_start (da Parsed_metadata_1) sia prima di lavfi.black_end (da Parsed_metadata_2)
			//
			//	[Parsed_metadata_1 @ 0x...] lavfi.black_start=12.48
			//  viene stampato sul primo frame nero e un nero non era già in corso. La durata minima d qui non conta.
			//
			//	[Parsed_metadata_2 @ 0x...] lavfi.black_end=15.6
			//	viene stampato sul primo frame non nero dopo un nero. Anche questa esce sempre, indipendentemente da d.
			//
			//	[Parsed_blackdetect_0 @ 0x55d4c8a0] black_start:12.48 black_end:15.6 black_duration:3.12
			//	lo stampa blackdetect quando il nero finisce, solo se black_duration >= d.
			//  È generata sullo stesso frame di lavfi.black_end. Dato che blackdetect elabora il frame prima di passarlo a metadata,
			//  compare prima delle righe di Parsed_metadata_2.
			//  Viene stampato anche a fine stream se il nero era ancora in corso e ha già superato d.
			//  In quel caso però lavfi.black_end non compare, perché non arriva nessun frame non nero.
			// FFMpegEngine::run ricava d da "blackdetect=d=", per cui il formato non va cambiato
			filter = std::format("blackdetect=d={:.2f}:pix_th={:.2f}"
				",metadata=mode=print:key=lavfi.black_start,metadata=mode=print:key=lavfi.black_end",
				black_min_duration, pixel_black_th);

			break;
		}
		case "blackframe"_case:
		{
			auto amount = JsonPath(&filterRoot)["amount"].as<int32_t>(98);
			auto threshold = JsonPath(&filterRoot)["threshold"].as<int32_t>(32);

			filter = std::format("blackframe=amount={}:threshold={}", amount, threshold);

			break;
		}
		case "crop"_case:
		{
			// x,y 0,0 indica il punto in basso a sinistra del video
			// in_h e in_w indicano input width and height
			auto out_w = JsonPath(&filterRoot)["out_w"].as<string>("in_w");
			auto out_h = JsonPath(&filterRoot)["out_h"].as<string>("in_h");
			// La posizione orizzontale, nel video di input, del bordo sinistro del video di output
			auto x = JsonPath(&filterRoot)["x"].as<string>("(in_w-out_w)/2");
			// La posizione verticale, nel video in input, del bordo superiore del video in output
			auto y = JsonPath(&filterRoot)["y"].as<string>("(in_h-out_h)/2");
			auto keep_aspect = JsonPath(&filterRoot)["keep_aspect"].as<bool>(false);
			// Enable exact cropping. If enabled, subsampled videos will be cropped at exact width/height/x/y
			// as specified and will not be rounded to nearest smaller value
			auto exact = JsonPath(&filterRoot)["exact"].as<bool>(false);

			// crop=w=100:h=100:x=12:y=34
			filter = std::format("crop=out_w={}:out_h={}:x={}:y={}:keep_aspect={}:exact={}", out_w, out_h, x, y,
				keep_aspect, exact);

			break;
		}
		case "drawbox"_case:
		{
			// x,y 0,0 indica il punto in basso a sinistra del video
			// in_h e in_w indicano input width and height
			auto x = JsonPath(&filterRoot)["x"].as<string>("0");
			auto y = JsonPath(&filterRoot)["y"].as<string>("0");
			auto width = JsonPath(&filterRoot)["width"].as<string>("300");
			auto height = JsonPath(&filterRoot)["height"].as<string>("300");
			auto fontColor = JsonPath(&filterRoot)["fontColor"].as<string>("red");
			auto percentageOpacity = JsonPath(&filterRoot)["percentageOpacity"].as<int32_t>(-1);
			// thickness: il valore speciale di "fill" riempie il box
			auto thickness = JsonPath(&filterRoot)["thickness"].as<string>("3");

			std::string opacity;
			if (percentageOpacity != -1)
			{
				// char cOpacity[64];

				// sprintf(cOpacity, "%.1f", ((float)percentageOpacity) / 100.0);
				// opacity = ("@" + string(cOpacity));
				opacity = std::format("@{:.1}", ((float)percentageOpacity) / 100.0);
			}

			// drawbox=x=700:y=400:w=160:h=90:color=blue:t=5
			filter = std::format("drawbox=x={}:y={}:w={}:h={}:color={}{}:t={}", x, y, width, height,
				fontColor, opacity, thickness);

			break;
		}
		case "drawtext"_case:
		{
			auto text = JsonPath(&filterRoot)["text"].as<string>("");
			// timecode: none, editorial, pts
			auto timecode = JsonPath(&filterRoot)["timecode"].as<string>("none");
			auto reloadAtFrameInterval = JsonPath(&filterRoot)["reloadAtFrameInterval"].as<int32_t>(-1);

			auto textPosition_X_InPixel = JsonPath(&filterRoot)["textPosition_X_InPixel"].as<string>("");
			auto textPosition_Y_InPixel = JsonPath(&filterRoot)["textPosition_Y_InPixel"].as<string>("");
			auto fontType = JsonPath(&filterRoot)["fontType"].as<string>("");
			auto fontSize = JsonPath(&filterRoot)["fontSize"].as<int32_t>(-1);
			auto fontColor = JsonPath(&filterRoot)["fontColor"].as<string>("");
			auto textPercentageOpacity = JsonPath(&filterRoot)["textPercentageOpacity"].as<int32_t>(-1);
			auto shadowX = JsonPath(&filterRoot)["shadowX"].as<int32_t>(0);
			auto shadowY = JsonPath(&filterRoot)["shadowY"].as<int32_t>(0);
			auto boxEnable = JsonPath(&filterRoot)["boxEnable"].as<bool>(false);
			auto boxColor = JsonPath(&filterRoot)["boxColor"].as<string>("");
			auto boxPercentageOpacity = JsonPath(&filterRoot)["boxPercentageOpacity"].as<int32_t>(-1);
			auto boxBorderW = JsonPath(&filterRoot)["boxBorderW"].as<int32_t>(0);

			/* TIMECODE
			1) editorialTimecode: è un’informazione “editoriale”, non tecnica per la riproduzione. Questo timecode NON è usato internamente
				dal player o dal decoder video. E' usato in ambito broadcast, editing e post-produzione.
				Formato tipico SMPTE: HH:MM:SS:FF, FF = frame number (dipende dal frame rate)
				Indica un orario leggibile (es. 10:03:05:12) che rappresenta la posizione temporale del frame all’interno di un contenuto.
				Si trova nel file o nel flusso:
				- come metadata del flusso video (tag timecode=10:03:05:00);
				- o incorporato nel segnale SDI (VITC/LTC);
				- oppure codificato come stream separato (es. timecode stream)
			2) ptsTimecode: è il timecode tecnico basato sui PTS (Presentation Time Stamp) del flusso video. Questo timecode è usato
				dal player o dal decoder video per la riproduzione. Indica quando un frame (video o audio) deve essere mostrato o riprodotto.
				Si trova nel transport stream (es. MPEG-TS) o nel container (es. MP4) — per ogni pacchetto o frame.
				Serve a:
				- Mantenere la sincronia tra audio e video.
				- Ricostruire la temporizzazione corretta in decodifica.
				- Fare seeking e tagli precisi nei file.
			3) DTS (Decoding Time Stamp) è un altro tipo di timestamp usato per indicare quando un frame deve essere decodificato.
				Non sempre è uguale al PTS, specialmente nei flussi con B-frames. Infatti serve soprattutto per codec con B-frame (bidirezionali),
				dove l’ordine di decodifica ≠ ordine di presentazione.

			Scenario AWS con MediaLive e timecode insertion PIC_TIMING_SEI
				Se il flusso in input contiene il timecode nel SEI pic_timing, AWS con Media Live è in grado di leggerlo e usarlo per l’inserimento
				del timecode nel video in output.
				FFMpeg invece non puo leggerlo direttamente in drawtext perchè drawtext può leggere solo:
				- metadata del container
				- metadata del frame (se FFmpeg li mappa)
				- variabili interne (pts, t, n, ecc.)
				- SMPTE timecode presente come metadata timecode
				- un file esterno
				- variabili LTC/ATC tramite asetpts
			Ma il SEI pic_timing di solito non viene esposto come metadata timecode.
			*/

			{
				std::string textFilePathName;
				// Serve un file se:
				// - è presente reloadAtFrameInterval
				// - sono presenti caratteri speciali che possono creare problemi nell'espressione drawtext
				if (reloadAtFrameInterval > 0 || text.find_first_of("'%:\\\r\n") != std::string::npos)
					textFilePathName = getDrawTextTemporaryPathName(_ffmpegTempDir, _ingestionJobKey, _encodingJobKey, _outputIndex);

				// 2026-10-08: in caso di file, non c'è bisogno di escape tranne se si usa % come letterale,
				// in tal caso bisogna fare l'escape con \%
				std::string escape;
				if (textFilePathName.empty())
					escape = "\\";
				else
				{
					// 2026-10-08: in caso di file, non c'è bisogno di escape tranne se si usa % come letterale.
					// Ad esempio ho trovato un caso particolare con %% e l'ho gestito con un replace di % con \%
					// per evitare che ffmpeg interpreti % come carattere speciale e dia errore.
					text = StringUtils::replaceAll(text, "%%", "\\%\\%");
				}

				text = StringUtils::replaceAll(text, ":", std::format("{}:", escape));
				text = StringUtils::replaceAll(text, "'", std::format("{}'", escape));

				if (inputDurationInSeconds)
				{
					// see https://ffmpeg.org/ffmpeg-filters.html
					// see https://ffmpeg.org/ffmpeg-utils.html
					//
					// expr_int_format, eif
					//	Evaluate the expression’s value and output as formatted integer.
					//	The first argument is the expression to be evaluated, just as for the expr function.
					//	The second argument specifies the output format. Allowed values are ‘x’, ‘X’, ‘d’ and ‘u’. They are treated exactly as in the
					// printf function. 	The third parameter is optional and sets the number of positions taken by the output. It can be used to add
					// padding with zeros from the left.
					//

					{
						text = StringUtils::replaceAll(text, "days_counter",
							"%{eif" + escape + ":trunc((countDownDurationInSecs-t)/86400)" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "hours_counter",
							"%{eif" + escape + ":trunc(mod(((countDownDurationInSecs-t)/3600),24))" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "hours_counter",
							"%{eif" + escape + ":trunc(mod(((countDownDurationInSecs-t)/3600),24))" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "mins_counter",
							"%{eif" + escape + ":trunc(mod(((countDownDurationInSecs-t)/60),60))" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "secs_counter",
							"%{eif" + escape + ":trunc(mod(countDownDurationInSecs-t" + escape + ",60))" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "cents_counter",
							"%{eif" + escape + ":(mod(countDownDurationInSecs-t" + escape + ",1)*pow(10,2))" + escape + ":d" + escape + ":2}"
						);
						text = StringUtils::replaceAll(text, "countDownDurationInSecs", std::format("{}", *inputDurationInSeconds));
					}
				}

				if (timecode == "editorialTimecode" && text.find(std::format("%{{metadata{}:timecode}}", escape)) == std::string::npos)
				{
					if (!text.empty())
						text += ' ';
					text += std::format("%{{metadata{}:timecode}}", escape);
				}
				else if (timecode == "ptsTimecode" && text.find(std::format("%{{pts{}:", escape)) == std::string::npos)
				{
					if (!text.empty())
						text += ' ';
					// text += "time: %{localtime:%Y-%m-%d %H.%M.%S}";
					// text += "time: %{pts:localtime}";

					// genera la date/time utc con formato 2025-11-15 14:30:00
					time_t utcTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
					text += std::format("%{{pts{}:gmtime{}:{}}}", escape, escape, utcTime);

					// parte da 00:00:00.000
					// text += std::format("%{{pts{}:hms}}", escape);
				}

				if (!textFilePathName.empty())
				{
					// questo file dovrà essere rimosso dallo script di retention
					std::ofstream of(textFilePathName, std::ofstream::trunc);
					of << text;
					of.flush();
				}

				/*
				* -vf "drawtext=fontfile='C\:\\Windows\\fonts\\Arial.ttf':
				fontcolor=yellow:fontsize=45:x=100:y=65:
				text='%{eif\:trunc((5447324-t)/86400)\:d\:2} days
				%{eif\:trunc(mod(((5447324-t)/3600),24))\:d\:2} hrs
				%{eif\:trunc(mod(((5447324-t)/60),60))\:d\:2} m
				%{eif\:trunc(mod(5447324-t\,60))\:d\:2} s'"

				* 5447324 is the countdown duration expressed in seconds
				*/
				std::string ffmpegTextPosition_X_InPixel;
				switch (hash_case(textPosition_X_InPixel))
				{
					case "left"_case:
					{
						ffmpegTextPosition_X_InPixel = "20";
						break;
					}
					case "center"_case:
					{
						ffmpegTextPosition_X_InPixel = "(w - text_w)/2";
						break;
					}
					case "right"_case:
					{
						ffmpegTextPosition_X_InPixel = "w - (text_w + 20)";
						break;
					}

					// t (timestamp): 0, 1, 2, ...
					case "leftToRight_5"_case:
					{
						ffmpegTextPosition_X_InPixel = "(5 * t) - text_w";
						break;
					}
					case "leftToRight_10"_case:
					{
						ffmpegTextPosition_X_InPixel = "(10 * t) - text_w";
						break;
					}
					case "loopLeftToRight_5"_case:
					{
						ffmpegTextPosition_X_InPixel = "mod(5 * t\\, w + text_w) - text_w";
						break;
					}
					case "loopLeftToRight_10"_case:
					{
						ffmpegTextPosition_X_InPixel = "mod(10 * t\\, w + text_w) - text_w";
						break;
					}

					// 15 and 30 sono stati decisi usando un video 1920x1080
					case "rightToLeft_15"_case:
					{
						ffmpegTextPosition_X_InPixel = "w - (((w + text_w) / 15) * t)";
						break;
					}
					case "rightToLeft_30"_case:
					{
						ffmpegTextPosition_X_InPixel = "w - (((w + text_w) / 30) * t)";
						break;
					}

					// 2026-01-21: rivisti i calcoli per loopRightToLeft
					case "loopRightToLeft_15"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*15\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_30"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*30\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_60"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*60\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_90"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*90\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_120"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*120\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_150"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*150\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_180"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*180\\,w+text_w)";
						break;
					}
					case "loopRightToLeft_210"_case:
					{
						ffmpegTextPosition_X_InPixel = "w-mod(t*210\\,w+text_w)";
						break;
					}
					default:
					{
						ffmpegTextPosition_X_InPixel = StringUtils::replaceAll(textPosition_X_InPixel, ",", std::format("{},", escape));

						ffmpegTextPosition_X_InPixel = StringUtils::replaceAll(ffmpegTextPosition_X_InPixel, "video_width", "w");
						ffmpegTextPosition_X_InPixel = StringUtils::replaceAll(ffmpegTextPosition_X_InPixel, "text_width", "text_w"); // text_w or tw
						ffmpegTextPosition_X_InPixel = StringUtils::replaceAll(ffmpegTextPosition_X_InPixel, "line_width", "line_w");
						ffmpegTextPosition_X_InPixel = StringUtils::replaceAll(ffmpegTextPosition_X_InPixel, "timestampInSeconds", "t");
					}
				}

				std::string ffmpegTextPosition_Y_InPixel;
				switch (hash_case(textPosition_X_InPixel))
				{
					case "below"_case:
					{
						ffmpegTextPosition_Y_InPixel = "h - (text_h + 20)";
						break;
					}
					case "center"_case:
					{
						ffmpegTextPosition_Y_InPixel = "(h - text_h)/2";
						break;
					}
					case "high"_case:
					{
						ffmpegTextPosition_Y_InPixel = "20";
						break;
					}

					// t (timestamp): 0, 1, 2, ...
					case "bottomToTop_50"_case:
					{
						ffmpegTextPosition_Y_InPixel = "h - (t * 50)";
						break;
					}
					case "bottomToTop_100"_case:
					{
						ffmpegTextPosition_Y_InPixel = "h - (t * 100)";
						break;
					}
					case "loopBottomToTop_50"_case:
					{
						ffmpegTextPosition_Y_InPixel = "h - mod(t * 50\\, h)";
						break;
					}
					case "loopBottomToTop_100"_case:
					{
						ffmpegTextPosition_Y_InPixel = "h - mod(t * 100\\, h)";
						break;
					}

					case "topToBottom_50"_case:
					{
						ffmpegTextPosition_Y_InPixel = "t * 50";
						break;
					}
					case "topToBottom_100"_case:
					{
						ffmpegTextPosition_Y_InPixel = "t * 100";
						break;
					}
					case "loopTopToBottom_50"_case:
					{
						ffmpegTextPosition_Y_InPixel = "mod(t * 50\\, h)";
						break;
					}
					case "loopTopToBottom_100"_case:
					{
						ffmpegTextPosition_Y_InPixel = "mod(t * 100\\, h)";
						break;
					}
					default:
					{
						ffmpegTextPosition_Y_InPixel = StringUtils::replaceAll(textPosition_Y_InPixel, ",", std::format("{},", escape));

						ffmpegTextPosition_Y_InPixel = StringUtils::replaceAll(ffmpegTextPosition_Y_InPixel, "video_height", "h");
						ffmpegTextPosition_Y_InPixel = StringUtils::replaceAll(ffmpegTextPosition_Y_InPixel, "text_height", "text_h");
						ffmpegTextPosition_Y_InPixel = StringUtils::replaceAll(ffmpegTextPosition_Y_InPixel, "line_height", "line_h");
						ffmpegTextPosition_Y_InPixel = StringUtils::replaceAll(ffmpegTextPosition_Y_InPixel, "timestampInSeconds", "t");
					}
				}

				{
					if (!textFilePathName.empty())
					{
						filter = std::format("drawtext=textfile='{}'", textFilePathName);
						if (reloadAtFrameInterval > 0)
							filter += std::format(":reload={}", reloadAtFrameInterval);
					}
					else
						filter = std::format("drawtext=text='{}'", text);
				}
				if (!textPosition_X_InPixel.empty())
					filter += (":x=" + ffmpegTextPosition_X_InPixel);
				if (!textPosition_Y_InPixel.empty())
					filter += (":y=" + ffmpegTextPosition_Y_InPixel);
				if (!fontType.empty())
					filter += std::format(":fontfile='{}/{}'", _ffmpegTtfFontDir, fontType);
				if (fontSize != -1)
					filter += std::format(":fontsize={}", fontSize);
				if (!fontColor.empty())
				{
					filter += (":fontcolor=" + fontColor);
					if (textPercentageOpacity != -1)
					{
						/*
						char opacity[64];

						sprintf(opacity, "%.1f", ((float)textPercentageOpacity) / 100.0);

						filter += ("@" + string(opacity));
						*/
						filter += std::format("@{:.1}", ((float)textPercentageOpacity) / 100.0);
					}
				}
				filter += std::format(":shadowx={}", shadowX);
				filter += std::format(":shadowy={}", shadowY);
				if (boxEnable)
				{
					filter += (":box=1");

					if (!boxColor.empty())
					{
						filter += (":boxcolor=" + boxColor);
						if (boxPercentageOpacity != -1)
						{
							/*
							char opacity[64];

							sprintf(opacity, "%.1f", ((float)boxPercentageOpacity) / 100.0);

							filter += ("@" + string(opacity));
							*/
							filter += std::format("@{:.1}", ((float)boxPercentageOpacity) / 100.0);
						}
					}
					if (boxBorderW != -1)
						filter += std::format(":boxborderw={}", boxBorderW);
				}
			}

			LOG_INFO(
				"getDrawTextVideoFilterDescription"
				", text: {}"
				", textPosition_X_InPixel: {}"
				", textPosition_Y_InPixel: {}"
				", fontType: {}"
				", fontSize: {}"
				", fontColor: {}"
				", textPercentageOpacity: {}"
				", boxEnable: {}"
				", boxColor: {}"
				", boxPercentageOpacity: {}"
				", streamingDurationInSeconds: {}"
				", filter: {}",
				text, textPosition_X_InPixel, textPosition_Y_InPixel, fontType, fontSize, fontColor, textPercentageOpacity, boxEnable, boxColor,
				boxPercentageOpacity, inputDurationInSeconds ? *inputDurationInSeconds : -1, filter
			);

			break;
		}
		case "fade"_case:
		{
			auto duration = JsonPath(&filterRoot)["duration"].as<int32_t>(4);

			if (inputDurationInSeconds && *inputDurationInSeconds >= duration)
			{
				// fade=type=in:duration=3,fade=type=out:duration=3:start_time=27
				filter = std::format("fade=type=in:duration={},fade=type=out:duration={}:start_time={}",
					duration, duration, *inputDurationInSeconds - duration);
				// ("fade=type=in:duration=" + to_string(duration) + ",fade=type=out:duration=" + to_string(duration) +
				//  ":start_time=" + to_string(streamingDurationInSeconds - duration));
			}
			else
			{
				LOG_WARN(
					"fade filter, streaming duration to small"
					", fadeDuration: {}"
					", streamingDurationInSeconds: {}",
					duration, inputDurationInSeconds ? *inputDurationInSeconds : -1
				);
			}

			break;
		}
		case "fps"_case:
		{
			auto framesNumber = JsonPath(&filterRoot)["framesNumber"].as<int32_t>(25);
			auto periodInSeconds = JsonPath(&filterRoot)["periodInSeconds"].as<int32_t>(1);

			filter = std::format("fps={}/{}", framesNumber, periodInSeconds);

			break;
		}
		case "freezedetect"_case:
		{
			auto noiseInDb = JsonPath(&filterRoot)["noiseInDb"].as<int32_t>(-60);
			auto duration = JsonPath(&filterRoot)["duration"].as<int32_t>(2);

			filter = std::format("freezedetect=noise={}dB:duration={}", noiseInDb, duration);

			break;
		}
		case "imageoverlay"_case:
		{
			auto imagePosition_X_InPixel = JsonPath(&filterRoot)["imagePosition_X_InPixel"].as<string>("0");
			auto imagePosition_Y_InPixel = JsonPath(&filterRoot)["imagePosition_Y_InPixel"].as<string>("0");

			std::string ffmpegImagePosition_X_InPixel;
			if (imagePosition_X_InPixel == "left")
				ffmpegImagePosition_X_InPixel = "20";
			else if (imagePosition_X_InPixel == "center")
				ffmpegImagePosition_X_InPixel = "(main_w - overlay_w)/2";
			else if (imagePosition_X_InPixel == "right")
				ffmpegImagePosition_X_InPixel = "main_w - (overlay_w + 20)";
			else
			{
				// ffmpegImagePosition_X_InPixel = regex_replace(imagePosition_X_InPixel, regex("video_width"), "main_w");
				ffmpegImagePosition_X_InPixel = StringUtils::replaceAll(imagePosition_X_InPixel, "video_width", "main_w");
				// ffmpegImagePosition_X_InPixel = regex_replace(ffmpegImagePosition_X_InPixel, regex("image_width"), "overlay_w");
				ffmpegImagePosition_X_InPixel = StringUtils::replaceAll(ffmpegImagePosition_X_InPixel, "image_width", "overlay_w");
			}

			std::string ffmpegImagePosition_Y_InPixel;
			if (imagePosition_Y_InPixel == "below")
				ffmpegImagePosition_Y_InPixel = "main_h - (overlay_h + 20)";
			else if (imagePosition_Y_InPixel == "center")
				ffmpegImagePosition_Y_InPixel = "(main_h - overlay_h)/2";
			else if (imagePosition_Y_InPixel == "high")
				ffmpegImagePosition_Y_InPixel = "20";
			else
			{
				// ffmpegImagePosition_Y_InPixel = regex_replace(imagePosition_Y_InPixel, regex("video_height"), "main_h");
				ffmpegImagePosition_Y_InPixel = StringUtils::replaceAll(imagePosition_Y_InPixel, "video_height", "main_h");
				// ffmpegImagePosition_Y_InPixel = regex_replace(ffmpegImagePosition_Y_InPixel, regex("image_height"), "overlay_h");
				ffmpegImagePosition_Y_InPixel = StringUtils::replaceAll(ffmpegImagePosition_Y_InPixel, "image_height", "overlay_h");
			}

			// overlay=x=main_w-overlay_w-10:y=main_h-overlay_h-10
			filter = std::format("overlay=x={}:y={}", ffmpegImagePosition_X_InPixel, ffmpegImagePosition_Y_InPixel);

			break;
		}
		case "metadata"_case:
		{
			filter = ("metadata=mode=print");

			break;
		}
		case "select"_case:
		{
			// select frames to pass in output
			auto frameType = JsonPath(&filterRoot)["frameType"].as<string>("i-frame");

			// es: vfr
			auto fpsMode = JsonPath(&filterRoot)["fpsMode"].as<string>("");

			if (frameType == "i-frame")
				filter = "select='eq(pict_type,PICT_TYPE_I)'";
			else if (frameType == "scene")
			{
				// double between 0 and 1. 0.5: 50% of changes
				auto changePercentage = JsonPath(&filterRoot)["changePercentage"].as<double>(0.5);

				filter = std::format("select='eq(scene,{})'", changePercentage);
			}
			else
			{
				std::string errorMessage = "filterRoot->frameType is unknown";
				LOG_ERROR(errorMessage);

				throw std::runtime_error(errorMessage);
			}

			if (!fpsMode.empty())
				filter += std::format(" -fps_mode {}", fpsMode);

			break;
		}
		case "showinfo"_case:
		{
			filter = ("showinfo");

			break;
		}
		case "silencedetect"_case:
		{
			auto noise = JsonPath(&filterRoot)["noise"].as<double>(0.0001);

			filter = std::format("silencedetect=noise={}", noise);

			break;
		}
		case "volume"_case:
		{
			auto factor = JsonPath(&filterRoot)["factor"].as<double>(5.0);

			filter = std::format("volume={}", factor);

			break;
		}
		default:
		{
			std::string errorMessage = std::format(
				"filterRoot->type is unknown"
				", type: {}",
				type
			);
			LOG_ERROR(errorMessage);

			throw std::runtime_error(errorMessage);
		}
	}

	return filter;
}

nlohmann::json FFMpegFilters::mergeFilters(const nlohmann::json& filters_1Root, const nlohmann::json& filters_2Root)
{

	nlohmann::json mergedFiltersRoot = nullptr;

	if (filters_1Root == nullptr)
		mergedFiltersRoot = filters_2Root;
	else if (filters_2Root == nullptr)
		mergedFiltersRoot = filters_1Root;
	else
	{
		std::string field = "video";
		{
			if (JSONUtils::isPresent(filters_1Root, field))
				mergedFiltersRoot[field] = filters_1Root[field];

			if (JSONUtils::isPresent(filters_2Root, field))
			{
				for (int filterIndex = 0; filterIndex < filters_2Root[field].size(); filterIndex++)
					mergedFiltersRoot[field].push_back(filters_2Root[field][filterIndex]);
			}
		}

		field = "audio";
		{
			if (JSONUtils::isPresent(filters_1Root, field))
				mergedFiltersRoot[field] = filters_1Root[field];

			if (JSONUtils::isPresent(filters_2Root, field))
			{
				for (int filterIndex = 0; filterIndex < filters_2Root[field].size(); filterIndex++)
					mergedFiltersRoot[field].push_back(filters_2Root[field][filterIndex]);
			}
		}

		field = "complex";
		{
			if (JSONUtils::isPresent(filters_1Root, field))
				mergedFiltersRoot[field] = filters_1Root[field];

			if (JSONUtils::isPresent(filters_2Root, field))
			{
				for (int filterIndex = 0; filterIndex < filters_2Root[field].size(); filterIndex++)
					mergedFiltersRoot[field].push_back(filters_2Root[field][filterIndex]);
			}
		}
	}

	return mergedFiltersRoot;
}

std::string FFMpegFilters::getDrawTextTemporaryPathName(const std::string& ffmpegTempDir, int64_t ingestionJobKey,
	int64_t encodingJobKey, int outputIndex)
{
	if (outputIndex != -1)
		return std::format("{}/{}_{}_{}.overlayText", ffmpegTempDir, ingestionJobKey, encodingJobKey, outputIndex);
	return std::format("{}/{}_{}.overlayText", ffmpegTempDir, ingestionJobKey, encodingJobKey);
}

nlohmann::json FFMpegFilters::createTimecodeDrawTextFilter()
{
	nlohmann::json drawTextFilterRoot;
	drawTextFilterRoot["type"] = "drawtext";
	{
		drawTextFilterRoot["timecode"] = "ptsTimecode";
		drawTextFilterRoot["textPosition_X_InPixel"] = "center";
		drawTextFilterRoot["textPosition_Y_InPixel"] = "center";
		drawTextFilterRoot["fontType"] = "OpenSans-ExtraBold.ttf";
		drawTextFilterRoot["fontSize"] = 48;
		drawTextFilterRoot["fontColor"] = "orange";
		drawTextFilterRoot["textPercentageOpacity"] = 100;
		drawTextFilterRoot["shadowX"] = 0;
		drawTextFilterRoot["shadowY"] = 0;
		drawTextFilterRoot["boxEnable"] = false;
	}

	return drawTextFilterRoot;
}