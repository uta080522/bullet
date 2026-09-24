/* White-box tests and offline stand-ins for the two download tools. */
#define BULLET_TEST
#include "bullet.c"

static const char gif[] =
    "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh"
    "+QQICgAAACwAAAAAAgACAAAIBgABCAQQEAAh+QQIFAAAACwAAAAAAgACAIEA"
    "AP8AAAAAAAAAAAAIBgABCAQQEAA7";

static void
expect(int condition, const char *what)
{
	if (!condition)
		die("test failed: %s", what);
}

static const char *
valueof(int argc, char **argv, const char *key)
{
	int i;

	for (i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], key))
			return argv[i + 1];
	die("test tool: missing %s", key);
}

static void
copyfile(const char *from, const char *to)
{
	unsigned char *data;
	size_t n;

	expect(from != NULL, "fixture path");
	data = readfile(from, MAX_JSON, &n);
	writefile(to, data, n);
	free(data);
}

static void
fake_tool(int argc, char **argv)
{
	const char *dir, *id, *record;
	char *path, *chat;
	SDL_IOStream *io;

	record = SDL_getenv("BULLET_TOOL_LOG");
	expect(record != NULL, "fake tool log");
	io = SDL_IOFromFile(record, "ab");
	check(io != NULL, "open fake tool log");
	writeall(io, argv[1], strlen(argv[1]));
	writeall(io, "\n", 1);
	check(SDL_CloseIO(io), "close fake tool log");
	if (SDL_getenv("BULLET_TOOL_FAIL") && *SDL_getenv("BULLET_TOOL_FAIL"))
		die("test downloader failed");
	if (!strcmp(argv[1], "chatdownload")) {
		copyfile(SDL_getenv("BULLET_FIXTURE_CHAT"),
			 valueof(argc, argv, "--output"));
		return;
	}
	dir = valueof(argc, argv, "-P");
	check(SDL_CreateDirectory(dir), "create fake download directory");
	if (strstr(argv[argc - 1], "twitch.tv")) {
		id = strrchr(argv[argc - 1], '/') + 1;
		path = format("%s/v%s.mp4", dir, id);
	} else {
		path = format("%s/fixture.mp4", dir);
		chat = format("%s/fixture.live_chat.json", dir);
		copyfile(SDL_getenv("BULLET_FIXTURE_YOUTUBE"), chat);
		free(chat);
	}
	if (!exists(path))
		copyfile(SDL_getenv("BULLET_FIXTURE_VIDEO"), path);
	puts(path);
	free(path);
}

static int
colour_x(int colour)
{
	int x, y, found, match;
	unsigned char *p;

	found = INT_MAX;
	for (y = 0; y < canvas->h; y++) {
		p = (unsigned char *)canvas->pixels + y * canvas->pitch;
		for (x = 0; x < canvas->w; x++) {
			if (!p[x * 4 + 3])
				continue;
			match = colour == 0
				    ? p[x * 4] > 240 && p[x * 4 + 1] < 10 &&
					  p[x * 4 + 2] < 10
				: colour == 1
				    ? p[x * 4 + 2] > 240 && p[x * 4] < 10 &&
					  p[x * 4 + 1] < 10
				    : p[x * 4] > 240 && p[x * 4 + 1] > 240 &&
					  p[x * 4 + 2] > 240;
			if (match && x < found)
				found = x;
			expect(p[x * 4 + 3] <= 128, "shared 50% opacity");
		}
	}
	expect(found != INT_MAX, "colour is present in rendered frame");
	return found;
}

static void
tests(void)
{
	SDL_Surface *a, *b;
	unsigned char *pixel;
	char hash[65], *destination, *cached;
	const char *tmp;
	cJSON *json;
	Message *m;
	size_t index, i;
	int r, w, blue, w2, num, den;
	int64_t utc;
	const char *youtube =
	    "{\"replayChatItemAction\":{\"videoOffsetTimeMsec\":\"0\","
	    "\"actions\":[{\"addChatItemAction\":{\"item\":{"
	    "\"liveChatTextMessageRenderer\":{\"timestampUsec\":\"500000\","
	    "\"message\":{\"simpleText\":\"waiting\"}}}}}]}}\n"
	    "{\"replayChatItemAction\":{\"videoOffsetTimeMsec\":\"2000\","
	    "\"actions\":[{\"addChatItemAction\":{\"item\":{"
	    "\"liveChatTextMessageRenderer\":{\"timestampUsec\":\"3000000\","
	    "\"message\":{\"runs\":[{\"text\":\"hello\"},{\"emoji\":{"
	    "\"isCustomEmoji\":true,\"image\":{\"thumbnails\":["
	    "{\"url\":\"https://example.com/a.png\",\"width\":32,"
	    "\"height\":16}]}}}]}}}}}]}}";

	rate("30000/1001", &num, &den);
	expect(num == 30000 && den == 1001, "rational fps");
	rate("113394000/3780913", &num, &den);
	expect(frame_time(10000000, num, den) == INT64_C(333431486674),
	       "large frame-rate terms do not overflow the clock");
	utc = rfc3339("2026-01-01T00:00:00.123456Z");
	expect(utc == rfc3339("2026-01-01T09:00:00.123456+09:00"),
	       "timezone offset");
	expect(rfc3339("2026-01-01T00:00:01Z") - utc == 876544,
	       "microsecond precision");
	hashurl("abc", hash);
	expect(!strcmp(hash, "ba7816bf8f01cfea414140de5dae2223"
			     "b00361a396177a9cb410ff61f20015ad"),
	       "SHA-256 cache names");

	a = surface(1, 1);
	b = surface(1, 1);
	pixel = b->pixels;
	pixel[0] = 255;
	pixel[3] = 128;
	paste(a, b, 0, 0);
	pixel = a->pixels;
	expect(pixel[0] == 255 && pixel[3] == 128, "straight alpha");
	pixel = b->pixels;
	pixel[0] = 0;
	pixel[2] = 255;
	paste(a, b, 0, 0);
	pixel = a->pixels;
	expect(abs(pixel[0] - 85) <= 1 && abs(pixel[2] - 170) <= 1,
	       "source-over colour");
	paste(a, b, -2, -2);
	paste(a, b, 2, 2);
	surface_free(a);
	surface_free(b);

	tmp = SDL_getenv("TEMP");
	if (!tmp)
		tmp = SDL_getenv("TMPDIR");
	if (!tmp)
		tmp = "/tmp";
	destination = format("%s/bullet-unit.mp4", tmp);
	beginwork(destination);
	free(destination);
	stage = format("%s/chat.json", workdir);
	writefile(stage, youtube, strlen(youtube));
	readchat(stage);
	expect(nmessages == 1 && messages[0].time == 2 * SECOND,
	       "YouTube pre-stream exclusion");
	expect(messages[0].count == 2 && nassets == 1 && assets[0].aspect == 2,
	       "mixed YouTube text and image");
	freechat();
	json = parsejson("{\"comments\":[{\"content_offset_seconds\":5,"
			 "\"created_at\":\"2026-01-01T00:00:06.100Z\","
			 "\"message\":{\"body\":\"hello\"}}]}");
	read_twitch(json);
	expect(messages[0].time == 5 * SECOND, "no Twitch interpolation");
	freechat();
	opt.hls = 1;
	opt.origin = rfc3339("2026-01-01T00:00:00Z");
	read_twitch(json);
	expect(messages[0].time == 6100000, "explicit HLS clock");
	cJSON_Delete(json);
	freechat();
	opt.hls = 0;

	opt.travel = 10 * SECOND;
	opt.opacity = 50;
	opt.font = SDL_getenv("BULLET_TEST_FONT");
	openfont(200, 80);
	index = asset("https://example.com/animated.gif", 1);
	assets[index].embedded = copystr(gif);
	load_asset(&assets[index], workdir);
	expect(assets[index].count == 2, "decode complete GIF");
	pixel = frame_at(&assets[index], 50000)->pixels;
	expect(pixel[0] == 255 && pixel[2] == 0, "first GIF frame");
	pixel = frame_at(&assets[index], 150000)->pixels;
	expect(pixel[0] == 0 && pixel[2] == 255, "second GIF frame");
	pixel = frame_at(&assets[index], 350000)->pixels;
	expect(pixel[0] == 255, "GIF loops");
	m = message(0);
	part(m, "M", NONE);
	part(m, "", index);
	expect(layout(200, 80) == 0, "free lane");
	canvas = surface(200, 80);
	drawframe(4850000, 0, 1);
	r = colour_x(0);
	w = colour_x(2);
	drawframe(4950000, 0, 1);
	blue = colour_x(1);
	w2 = colour_x(2);
	expect(r - blue == w - w2 && r > blue,
	       "text and animated image share position and time");
	opt.shadow = 1;
	surface_free(m->sprite);
	m->sprite = NULL;
	drawframe(4950000, 0, 1);
	expect(colour_x(1) == blue, "shadow does not move emote");
	freechat();
	index = asset("https://example.com/animated.gif", 1);
	load_asset(&assets[index], workdir);
	expect(assets[index].count == 2, "offline GIF cache reuse");
	hashurl(assets[index].url, hash);
	cached = format("%s/%s", workdir, hash);
	check(SDL_RemovePath(cached), "remove test cache");
	free(cached);
	freechat();
	opt.travel = default_travel;
	for (i = 0; i < 3; i++) {
		m = message(0);
		part(m, "hello", NONE);
	}
	expect(layout(120, 16) == 2, "overlap fallback");
	for (i = 0; i < 3; i++)
		expect(messages[i].time == 0 && messages[i].y == 0,
		       "crowding never postpones a comment");
	freechat();
	endwork();
	puts("unit: clock, JSON, alpha, GIF, cache, layout OK");
}

static unsigned int cli_checks;
static const char *cli_program;

static void
run_case(int success, const char *program, ...)
{
	const char *args[80], *arg;
	va_list ap;
	size_t n, i;
	int status;

	args[0] = program;
	n = 1;
	va_start(ap, program);
	while ((arg = va_arg(ap, const char *)) != NULL) {
		expect(n + 1 < sizeof args / sizeof *args, "argument limit");
		args[n++] = arg;
	}
	va_end(ap);
	args[n] = NULL;
	spawn(args, 0, 0, NULL);
	check(SDL_WaitProcess(child, true, &status), "wait for test command");
	SDL_DestroyProcess(child);
	child = NULL;
	if ((status == 0) != success ||
	    (!success && !strcmp(program, cli_program) && status != 1)) {
		for (i = 0; i < n; i++)
			fprintf(stderr, "%s ", args[i]);
		fputc('\n', stderr);
		die("unexpected exit %d", status);
	}
	cli_checks++;
}

static void
samebytes(const char *a, const char *b)
{
	unsigned char *x, *y;
	size_t nx, ny;

	x = readfile(a, MAX_JSON, &nx);
	y = readfile(b, MAX_JSON, &ny);
	expect(nx == ny && !memcmp(x, y, nx), "file contents preserved");
	free(x);
	free(y);
}

static void
test_env(const char *key, const char *value)
{
	expect(SDL_setenv_unsafe(key, value, 1) == 0,
	       "set process environment");
	check(
	    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), key, value, true),
	    "set SDL environment");
}

static char *
fixture(const char *name, const char *contents)
{
	char *path;

	path = format("%s/%s", workdir, name);
	if (contents)
		writefile(path, contents, strlen(contents));
	return path;
}

static void
audio_equal(const char *a, const char *b)
{
	const char *args[] = {"ffprobe",
			      "-v",
			      "error",
			      "-select_streams",
			      "a:0",
			      "-show_packets",
			      "-show_data_hash",
			      "sha256",
			      "-show_entries",
			      "packet=data_hash",
			      "-of",
			      "json",
			      NULL,
			      NULL};
	cJSON *x, *y;
	const cJSON *px, *py;
	char *text;

	args[12] = a;
	text = capture(args);
	x = parsejson(text);
	free(text);
	args[12] = b;
	text = capture(args);
	y = parsejson(text);
	free(text);
	px = field(x, "packets");
	py = field(y, "packets");
	expect(cJSON_IsArray(px) && px->child && cJSON_IsArray(py),
	       "audio exists");
	px = px->child;
	py = py->child;
	while (px && py) {
		expect(!strcmp(string(field(px, "data_hash")),
			       string(field(py, "data_hash"))),
		       "audio packet unchanged");
		px = px->next;
		py = py->next;
	}
	expect(!px && !py, "all audio packets preserved, including priming");
	cJSON_Delete(x);
	cJSON_Delete(y);
}

static SDL_EnumerationResult SDLCALL
remove_entry(void *unused, const char *dir, const char *name)
{
	SDL_PathInfo info;
	char *path;

	(void)unused;
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return SDL_ENUM_CONTINUE;
	path = format("%s/%s", dir, name);
	check(SDL_GetPathInfo(path, &info), "stat test file");
	if (info.type == SDL_PATHTYPE_DIRECTORY)
		check(SDL_EnumerateDirectory(path, remove_entry, NULL),
		      "clean test directory");
	check(SDL_RemovePath(path), "remove test file");
	free(path);
	return SDL_ENUM_CONTINUE;
}

static void
cli_tests(const char *bullet, const char *tool)
{
	const char *tmp, *test_font;
	char hash[65];
	char *destination, *source, *original, *chat, *result, *saved;
	char *backup, *log, *partial, *broken, *invalid, *portrait, *alias;
	char *yt, *tools, *dir, *cached, *path, *text, *other, *image;
	char *oldpath, *fake_bin, *fake_ffmpeg, *auto_video, *auto_chat;
	char *auto_output, *second_chat;
	unsigned char *data;
	size_t n, i;
	Video v;
	SDL_Surface *png;
	SDL_IOStream *io;
	const char *bad_options[][2] = {
	    {"--start", "-1"},
	    {"--start", "nan"},
	    {"--duration", "0"},
	    {"--opacity", "101"},
	    {"--fps", "0/0"},
	    {"--fps", "nan"},
	    {"--text-style", "other"},
	    {"--output-height", "3"},
	    {"--hls-start", "2026-02-30T00:00:00Z"},
	    {"--hls-start", "2026-01-01T00:00:00"}};
	const char *bad_json[] = {
	    "{}", "{\"comments\":[]}",
	    ("{\"comments\":[{\"content_offset_seconds\":-1,"
	     "\"message\":{\"body\":\"bad\"}}]}")};
	const char *twitch = "{\"FileInfo\" : {},\"comments\":[{"
			     "\"content_offset_seconds\":0,"
			     "\"created_at\":\"2026-01-01T00:00:00.5Z\","
			     "\"message\":{\"body\":\"test 日本語\"}}]}";
	const char *youtube =
	    "{\"replayChatItemAction\":{"
	    "\"videoOffsetTimeMsec\":\"0\",\"actions\":[{"
	    "\"addChatItemAction\":{\"item\":{\"liveChatTextMessageRenderer\":"
	    "{"
	    "\"message\":{\"runs\":[{\"text\":\"hello 日本語\"}]}}}}}]}}";
	const char *mixed =
	    "{\"comments\":[{\"content_offset_seconds\":0,"
	    "\"message\":{\"fragments\":[{\"text\":\"M \"},"
	    "{\"emoticon\":{\"emoticon_id\":\"1\"}},{\"text\":\" text \"},"
	    "{\"emoticon\":{\"emoticon_id\":\"2\"}}]}}]}";
#ifdef _WIN32
	wchar_t *wa, *wb;
#endif

	cli_program = bullet;
	tmp = SDL_getenv("TEMP");
	if (!tmp)
		tmp = "/tmp";
	destination = format("%s/bullet-cli.mp4", tmp);
	beginwork(destination);
	free(destination);
	source = fixture("元動画 ' & (source).mp4", NULL);
	original = fixture("original.mp4", NULL);
	chat = fixture("replay.data", twitch);
	result = fixture("result.mp4", NULL);
	saved = fixture("saved.mp4", NULL);
	invalid = fixture("invalid.mp4", NULL);
	test_font = SDL_getenv("BULLET_TEST_FONT");
	if (!test_font) {
		for (i = 0; i < sizeof fonts / sizeof *fonts; i++)
			if (exists(fonts[i])) {
				test_font = fonts[i];
				break;
			}
	}
	expect(test_font != NULL, "font for CLI tests");
	run_case(1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		 "testsrc2=s=160x90:r=30000/1001", "-f", "lavfi", "-i",
		 "sine=frequency=1000:sample_rate=48000", "-t", "2", "-c:v",
		 "libx264", "-g", "12", "-preset", "ultrafast", "-c:a", "aac",
		 "-movflags", "+faststart", source, NULL);
	copyfile(source, original);
	run_case(1, bullet, "--help", NULL);
	run_case(1, bullet, "--version", NULL);
	auto_video = fixture("auto.mp4", NULL);
	auto_chat = fixture("auto.chat.json", twitch);
	auto_output = fixture("auto.bullet.mp4", NULL);
	second_chat = fixture("auto.live_chat.json", NULL);
	copyfile(source, auto_video);
	run_case(1, bullet, "render", auto_video, "--font", test_font,
		 "--duration", "0.3", NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", auto_output,
		 "-f", "null", "-", NULL);
	run_case(0, bullet, "render", auto_video, "--font", test_font, NULL);
	writefile(second_chat, youtube, strlen(youtube));
	run_case(0, bullet, "render", auto_video, "--font", test_font,
		 "--force", NULL);
	run_case(1, bullet, "render", auto_video, auto_chat, "--duration",
		 "0.3", "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(auto_chat), "remove mock Twitch chat");
	run_case(1, bullet, "render", auto_video, "--duration", "0.3",
		 "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(second_chat), "remove mock YouTube chat");
	run_case(0, bullet, "render", auto_video, "--force", NULL);
	samebytes(auto_video, source);
	free(auto_video);
	free(auto_chat);
	free(auto_output);
	free(second_chat);
	run_case(0, bullet, "render", original, "--output", result, NULL);
	run_case(1, bullet, "render", source, chat, "--output", result,
		 "--font", test_font, NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", result, "-f",
		 "null", "-", NULL);
	v = probe(result);
	expect(v.fps_num == 30000 && v.fps_den == 1001, "CLI rational fps");
	audio_equal(source, result);
	copyfile(result, saved);
	run_case(0, bullet, "render", source, chat, "--output", result, NULL);
	samebytes(result, saved);
	run_case(1, bullet, "render", source, chat, "--output", result,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	run_case(0, bullet, "render", source, chat, "--output", source,
		 "--force", NULL);
	run_case(0, bullet, "render", source, chat, "--output", chat,
		 "--force", NULL);
	alias = fixture("hardlink.mp4", NULL);
#ifdef _WIN32
	wa = wide(alias);
	wb = wide(source);
	expect(CreateHardLinkW(wa, wb, NULL), "create test hardlink");
	SDL_free(wa);
	SDL_free(wb);
#else
	expect(link(source, alias) == 0, "create test hardlink");
#endif
	run_case(0, bullet, "render", source, chat, "--output", alias,
		 "--force", NULL);
	samebytes(source, alias);
	free(alias);

	backup = fixture("new.backup.mp4", NULL);
	copyfile(source, backup);
	log = fixture("new.ffmpeg.log", "untouched log");
	partial = fixture("new.part.mp4", "untouched partial");
	other = fixture("new.mp4", NULL);
	run_case(1, bullet, "render", backup, chat, "--output", other,
		 "--duration", "0.3", "--font", test_font, NULL);
	samebytes(backup, original);
	data = readfile(log, MAX_JSON, &n);
	expect(n == 13 && !memcmp(data, "untouched log", n), "unrelated log");
	free(data);
	data = readfile(partial, MAX_JSON, &n);
	expect(n == 17 && !memcmp(data, "untouched partial", n),
	       "unrelated part");
	free(data);
	writefile(log, twitch, strlen(twitch));
	run_case(1, bullet, "render", source, log, "--output", other,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	samebytes(log, chat);
	samebytes(backup, original);
	free(backup);
	free(log);
	free(partial);
	free(other);

	broken = fixture("broken.mp4", NULL);
	data = readfile(source, MAX_JSON, &n);
	writefile(broken, data, n * 2 / 3);
	free(data);
	probe(broken);
	run_case(0, "ffmpeg", "-v", "error", "-xerror", "-i", broken, "-f",
		 "null", "-", NULL);
	copyfile(result, saved);
	run_case(0, bullet, "render", broken, chat, "--output", result,
		 "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	free(broken);
	for (i = 0; i < sizeof bad_options / sizeof *bad_options; i++)
		run_case(0, bullet, "render", source, chat, "--output",
			 invalid, bad_options[i][0], bad_options[i][1], NULL);
	path = fixture("bad.json", NULL);
	for (i = 0; i < sizeof bad_json / sizeof *bad_json; i++) {
		writefile(path, bad_json[i], strlen(bad_json[i]));
		run_case(0, bullet, "render", source, path, "--output",
			 invalid, "--font", test_font, NULL);
	}
	free(path);
	expect(!exists(invalid), "failed render does not publish an output");
	run_case(1, bullet, "render", source, chat, "--output", result,
		 "--force", "--duration", "1", "--hls-start",
		 "2026-01-01T00:00:00Z", "--font", test_font, NULL);
	portrait = fixture("portrait.mp4", NULL);
	run_case(1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		 "color=s=144x256:r=12", "-t", "1", "-vf", "setsar=4/3",
		 "-c:v", "mpeg4", portrait, NULL);
	run_case(1, bullet, "render", portrait, chat, "--output", result,
		 "--force", "--duration", "0.5", "--font", test_font, NULL);
	v = probe(result);
	expect(v.width == 192 && v.height == 256, "portrait and SAR");
	free(portrait);

	/* All four image codecs and mixed animated/static rendering, offline.
	 */
	dir = fixture("assets", NULL);
	check(SDL_CreateDirectory(dir), "create fixture cache");
	data = unbase64(gif, &n);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/1/default/dark/2.0",
		hash);
	path = format("%s/%s", dir, hash);
	writefile(path, data, n);
	free(data);
	free(path);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/2/default/dark/2.0",
		hash);
	image = format("%s/%s", dir, hash);
	png = surface(2, 2);
	memset(png->pixels, 255, (size_t)png->pitch * png->h);
	io = SDL_IOFromFile(image, "wb");
	expect(io != NULL && IMG_SavePNG_IO(png, io, true), "PNG fixture");
	surface_free(png);
	path = fixture("mixed.json", mixed);
	run_case(1, bullet, "render", source, path, "--output", result,
		 "--force", "--duration", "1", "--travel-time", "1", "--font",
		 test_font, NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", result, "-f",
		 "null", "-", NULL);
	for (i = 0; i < 2; i++) {
		run_case(1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
			 "color=c=red:s=8x8", "-frames:v", "1", "-c:v",
			 i ? "libwebp" : "mjpeg", "-f", "image2", "-update",
			 "1", "-y", image, NULL);
		run_case(1, bullet, "render", source, path, "--output", result,
			 "--force", "--duration", "0.4", "--font", test_font,
			 NULL);
	}
	free(image);
	free(path);
	text = format("{\"comments\":[{\"content_offset_seconds\":0,"
		      "\"message\":{\"fragments\":[{\"emoticon\":{"
		      "\"emoticon_id\":\"broken\"}}]}}],\"embeddedData\":{"
		      "\"firstParty\":[{"
		      "\"id\":\"broken\",\"data\":\"%.*s\"}]}}",
		      (int)strlen(gif) - 16, gif);
	path = fixture("bad-gif.json", text);
	free(text);
	copyfile(result, saved);
	run_case(0, bullet, "render", source, path, "--output", result,
		 "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	free(path);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/broken/default/"
		"dark/2.0",
		hash);
	path = format("%s/%s", dir, hash);
	expect(!exists(path), "broken GIF never becomes a cached image");
	free(path);
	free(dir);

	yt = fixture("youtube.data", youtube);
	tools = fixture("tools.log", NULL);
	test_env("YT_DLP", tool);
	test_env("TWITCH_DOWNLOADER_CLI", tool);
	test_env("BULLET_FIXTURE_VIDEO", source);
	test_env("BULLET_FIXTURE_CHAT", chat);
	test_env("BULLET_FIXTURE_YOUTUBE", yt);
	test_env("BULLET_TOOL_LOG", tools);
	dir = fixture("youtube", NULL);
	run_case(1, bullet, "download",
		 "https://www.youtube.com/watch?v=fixture", "--dir", dir,
		 NULL);
	path = format("%s/fixture.mp4", dir);
	other = format("%s/fixture.live_chat.json", dir);
	samebytes(path, source);
	run_case(1, bullet, "render", path, other, "--output", result,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	free(path);
	free(other);
	free(dir);
	dir = fixture("twitch", NULL);
	run_case(1, bullet, "download", "https://www.twitch.tv/videos/123",
		 "--dir", dir, NULL);
	path = format("%s/v123.mp4", dir);
	cached = fixture("tools-before.log", NULL);
	copyfile(tools, cached);
	run_case(1, bullet, "download", "https://www.twitch.tv/videos/123",
		 "--dir", dir, NULL);
	samebytes(tools, cached);
	samebytes(path, source);
	writefile(path, "truncated", 9);
	run_case(0, bullet, "download", "https://www.twitch.tv/videos/123",
		 "--dir", dir, NULL);
	samebytes(tools, cached);
	test_env("BULLET_TOOL_FAIL", "1");
	run_case(0, bullet, "download", "https://www.twitch.tv/videos/456",
		 "--dir", dir, NULL);
	other = format("%s/v456.chat.json", dir);
	expect(!exists(other), "failed downloader does not publish chat");
	free(other);
	test_env("BULLET_TOOL_FAIL", "");
	run_case(0, bullet, "download", "http://www.youtube.com/watch?v=x",
		 "--dir", dir, NULL);
	run_case(0, bullet, "download", "https://example.com/video", "--dir",
		 dir, NULL);
	free(yt);
	free(tools);
	free(dir);
	free(path);
	free(cached);

	/* Force an early consumer exit to exercise broken-pipe cleanup. */
	fake_bin = fixture("fake-bin", NULL);
	check(SDL_CreateDirectory(fake_bin), "create test bin directory");
#ifdef _WIN32
	fake_ffmpeg = format("%s/ffmpeg.exe", fake_bin);
#else
	fake_ffmpeg = format("%s/ffmpeg", fake_bin);
#endif
	copyfile(tool, fake_ffmpeg);
#ifndef _WIN32
	expect(chmod(fake_ffmpeg, 0700) == 0, "executable test tool");
#endif
	oldpath = copystr(SDL_getenv("PATH"));
#ifdef _WIN32
	text = format("%s;%s", fake_bin, oldpath);
#else
	text = format("%s:%s", fake_bin, oldpath);
#endif
	test_env("PATH", text);
	copyfile(result, saved);
	run_case(0, bullet, "render", source, chat, "--output", result,
		 "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	test_env("PATH", oldpath);
	free(oldpath);
	free(text);
	free(fake_ffmpeg);
	free(fake_bin);
	samebytes(source, original);
	data = readfile(chat, MAX_JSON, &n);
	expect(n == strlen(twitch) && !memcmp(data, twitch, n),
	       "chat unchanged");
	free(data);
	free(source);
	free(original);
	free(chat);
	free(result);
	free(saved);
	free(invalid);
	check(SDL_EnumerateDirectory(workdir, remove_entry, NULL),
	      "clean CLI fixtures");
	endwork();
	printf("cli: %u native process checks passed; inputs unchanged\n",
	       cli_checks);
}

int
main(int argc, char **argv)
{
	int mode;

	atexit(cleanup);
	check(SDL_Init(0), "initialize SDL");
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		die("initialize HTTP library");
	if (argc > 1) {
		if (!strcmp(argv[1], "--version")) {
			puts("bullet offline test tool");
			return 0;
		}
		if (argc == 3 && !strcmp(argv[1], "--cli")) {
			cli_tests(argv[2], argv[0]);
			return 0;
		}
		if (!strcmp(argv[1], "chatdownload") ||
		    !strcmp(argv[1], "--no-playlist")) {
			fake_tool(argc, argv);
			return 0;
		}
		if (!strcmp(argv[1], "--help")) {
			usage();
			return 0;
		}
		mode = arguments(argc, argv);
		if (mode == 1)
			download();
		else
			render();
		return 0;
	}
	tests();
	return 0;
}
