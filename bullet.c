/* Replay chat, one clock, one RGBA layer. No window or rendering backend. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

/* Policy, not a configuration language. Times are microseconds. */
#define SECOND INT64_C(1000000)
#define MAX_TIME (7 * 86400 * SECOND)
#define MAX_JSON (64 * 1024 * 1024)
#define MAX_ASSET (8 * 1024 * 1024)
#define MAX_PIXELS (7680 * 4320)
#define MAX_MEMORY ((size_t)512 * 1024 * 1024)
#define MAX_MESSAGES 200000
#define MAX_PARTS 1000000
#define MAX_ASSETS 4096
#define MAX_FRAMES 500
#define SUBPIXEL_SCALE 65536U
#define NONE ((size_t)-1)

static const int64_t default_travel = 5 * SECOND;
static const int default_opacity = 50;
static const char *const fonts[] = {
    "C:/Windows/Fonts/meiryob.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};

typedef struct {
	unsigned char *data;
	size_t len, cap, limit;
} Buffer;

typedef struct {
	char *url, *embedded;
	double aspect;
	SDL_Surface **frames;
	int64_t *ends;
	size_t count;
	int needed;
} Asset;

typedef struct {
	char *text;
	size_t asset;
	int x, width;
} Part;

/* Static sprites own either a dense surface or nontransparent RGBA runs. */
typedef struct {
	SDL_Surface *pixels;
	unsigned char *runs;
	size_t bytes;
} Sprite;

typedef struct {
	int x, y, width;
} Run;

typedef struct {
	int64_t time, stamp;
	Part *parts;
	size_t count, order, next;
	int width, y;
	Sprite *sprite;
} Message;

typedef struct {
	int width, height, fps_num, fps_den;
	int64_t duration;
} Video;

/* Half-open interval of CFR frames and the occupied vertical RGBA band. */
typedef struct {
	int y, height;
	int64_t first, end;
	size_t visible;
} Overlay;

typedef struct {
	const char *video, *chat, *output, *font, *dir, *url;
	int64_t start, duration, travel, origin;
	int fps_num, fps_den, height, opacity, shadow, force, hls;
	int max_height;
} Options;

typedef struct {
	char *directory, *payload;
} CacheStage;

static Options opt;
static Message *messages;
static Asset *assets;
static size_t nmessages, nassets, nparts, surface_bytes;
static TTF_Font *font;
static SDL_Surface *canvas;
static SDL_Process *child;
static char *workdir, *stage, *asset_temp, *logpath;
static CacheStage cache_stage;
static char *inferred_chat, *inferred_output;
static int keep_log;
static const char *chat_context;
static int font_size, outline, gap, lane_height, emote_height;
#ifdef BULLET_TEST
static int dense_reference;
#endif

static SDL_NORETURN void
die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "bullet: ");
	if (chat_context)
		fprintf(stderr, "%s: ", chat_context);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void
check(int ok, const char *what)
{
	if (!ok)
		die("%s: %s", what, SDL_GetError());
}

static void *
resize(void *p, size_t n, size_t size)
{
	void *q;

	if (!size || n > MAX_MEMORY / size)
		die("allocation limit exceeded");
	q = realloc(p, n * size);
	if (!q)
		die("out of memory");
	return q;
}

static char *
copystr(const char *s)
{
	char *p;
	size_t n;

	n = strlen(s) + 1;
	p = resize(NULL, n, 1);
	memcpy(p, s, n);
	return p;
}

static char *
format(const char *fmt, ...)
{
	va_list ap;
	char *s, *copy;

	va_start(ap, fmt);
	if (SDL_vasprintf(&s, fmt, ap) < 0)
		die("out of memory");
	va_end(ap);
	copy = copystr(s);
	SDL_free(s);
	return copy;
}

static const char *
basenameof(const char *path)
{
	const char *p, *name;

	name = path;
	for (p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			name = p + 1;
	return name;
}

static char *
dirnameof(const char *path)
{
	char *s;
	size_t n;

	n = (size_t)(basenameof(path) - path);
	if (!n)
		return copystr(".");
	s = copystr(path);
	s[n] = '\0';
	return s;
}

static int
exists(const char *path)
{
	SDL_PathInfo info;

	return SDL_GetPathInfo(path, &info);
}

#ifdef _WIN32
static wchar_t *
wide(const char *s)
{
	wchar_t *w;

	w = (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", s, strlen(s) + 1);
	if (!w)
		die("invalid UTF-8 path");
	return w;
}
#endif

static int
samefile(const char *a, const char *b)
{
#ifdef _WIN32
	BY_HANDLE_FILE_INFORMATION x, y;
	HANDLE ha, hb;
	wchar_t *wa, *wb;
	int same;

	wa = wide(a);
	wb = wide(b);
	ha = CreateFileW(
	    wa, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    NULL, OPEN_EXISTING, 0, NULL);
	hb = CreateFileW(
	    wb, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    NULL, OPEN_EXISTING, 0, NULL);
	same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
	       GetFileInformationByHandle(ha, &x) &&
	       GetFileInformationByHandle(hb, &y) &&
	       x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
	       x.nFileIndexHigh == y.nFileIndexHigh &&
	       x.nFileIndexLow == y.nFileIndexLow;
	if (ha != INVALID_HANDLE_VALUE)
		CloseHandle(ha);
	if (hb != INVALID_HANDLE_VALUE)
		CloseHandle(hb);
	SDL_free(wa);
	SDL_free(wb);
	return same;
#else
	struct stat x, y;

	return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev &&
	       x.st_ino == y.st_ino;
#endif
}

static void
check_destination(void)
{
#ifdef _WIN32
	wchar_t *w;
	DWORD attr;

	w = wide(opt.output);
	attr = GetFileAttributesW(w);
	SDL_free(w);
	if (attr != INVALID_FILE_ATTRIBUTES &&
	    (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
		die("output must not be a link or directory");
#else
	struct stat st;

	if (!lstat(opt.output, &st) && !S_ISREG(st.st_mode))
		die("output must be a regular file");
#endif
	if (samefile(opt.output, opt.video) || samefile(opt.output, opt.chat))
		die("output cannot be the source video or chat");
	if (!opt.force && exists(opt.output))
		die("output exists; use another path or --force");
}

/* Both files are on the same filesystem. No fixed-name backup is used. */
static int
commitfile(const char *from, const char *to, int replace)
{
#ifdef _WIN32
	wchar_t *a, *b;
	int ok;

	a = wide(from);
	b = wide(to);
	ok = MoveFileExW(a, b,
			 MOVEFILE_WRITE_THROUGH |
			     (replace ? MOVEFILE_REPLACE_EXISTING : 0));
	SDL_free(a);
	SDL_free(b);
	return ok;
#else
	if (replace)
		return rename(from, to) == 0;
	if (link(from, to))
		return 0;
	return unlink(from) == 0;
#endif
}

static void
surface_free(SDL_Surface *s)
{
	if (s) {
		surface_bytes -= (size_t)s->pitch * s->h;
		SDL_DestroySurface(s);
	}
}

static void
surface_limit(int width, int height)
{
	if (width < 1 || height < 1 || width > 65536 || height > 16000 ||
	    (int64_t)width * height > MAX_PIXELS)
		die("image dimensions exceed limit: %dx%d", width, height);
	if ((size_t)width * height * 4 > MAX_MEMORY - surface_bytes)
		die("decoded image memory exceeds 512 MiB");
}

static SDL_Surface *
surface(int width, int height)
{
	SDL_Surface *s;

	surface_limit(width, height);
	s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
	check(s != NULL, "create RGBA surface");
	surface_bytes += (size_t)s->pitch * s->h;
	return s;
}

/* A NULL destination measures the packed size without allocating. */
static size_t
pack_runs(const SDL_Surface *s, unsigned char *data, size_t limit)
{
	const unsigned char *row;
	Run run;
	size_t size, bytes;
	int x, y;

	size = 0;
	for (y = 0; y < s->h; y++) {
		row = (const unsigned char *)s->pixels + y * s->pitch;
		for (x = 0; x < s->w;) {
			while (x < s->w && !row[x * 4 + 3])
				x++;
			run.x = x;
			while (x < s->w && row[x * 4 + 3])
				x++;
			if (x == run.x)
				continue;
			run.y = y;
			run.width = x - run.x;
			bytes = sizeof run + (size_t)run.width * 4;
			if (bytes > limit - size)
				return NONE;
			if (data) {
				memcpy(data + size, &run, sizeof run);
				memcpy(data + size + sizeof run,
				       row + run.x * 4, (size_t)run.width * 4);
			}
			size += bytes;
		}
	}
	return size;
}

/* Takes ownership. Keep dense pixels if packing cannot save memory or if
 * the temporary allocation would exceed the existing image budget. */
static Sprite *
sprite_create(SDL_Surface *pixels)
{
	Sprite *s;
	size_t limit, bytes;

	s = resize(NULL, 1, sizeof *s);
	memset(s, 0, sizeof *s);
	s->pixels = pixels;
#ifdef BULLET_TEST
	if (dense_reference)
		return s;
#endif
	limit = (size_t)pixels->pitch * pixels->h - 1;
	if (limit > MAX_MEMORY - surface_bytes)
		limit = MAX_MEMORY - surface_bytes;
	bytes = pack_runs(pixels, NULL, limit);
	if (bytes == NONE)
		return s;
	if (bytes) {
		s->runs = malloc(bytes);
		if (!s->runs)
			return s;
		if (pack_runs(pixels, s->runs, bytes) != bytes)
			die("sprite run sizes disagree");
	}
	s->bytes = bytes;
	surface_bytes += bytes;
	surface_free(pixels);
	s->pixels = NULL;
	return s;
}

static void
sprite_free(Sprite *s)
{
	if (s) {
		surface_free(s->pixels);
		surface_bytes -= s->bytes;
		free(s->runs);
		free(s);
	}
}

static void
freechat(void)
{
	size_t i, j;

	for (i = 0; i < nmessages; i++) {
		for (j = 0; j < messages[i].count; j++)
			free(messages[i].parts[j].text);
		free(messages[i].parts);
		sprite_free(messages[i].sprite);
	}
	for (i = 0; i < nassets; i++) {
		free(assets[i].url);
		free(assets[i].embedded);
		for (j = 0; j < assets[i].count; j++)
			surface_free(assets[i].frames[j]);
		free(assets[i].frames);
		free(assets[i].ends);
	}
	free(messages);
	free(assets);
	messages = NULL;
	assets = NULL;
	nmessages = nassets = nparts = 0;
}

static int
end_cache_stage(void)
{
	if (cache_stage.payload && !SDL_RemovePath(cache_stage.payload) &&
	    exists(cache_stage.payload))
		return 0;
	if (cache_stage.directory && !SDL_RemovePath(cache_stage.directory))
		return 0;
	free(cache_stage.payload);
	free(cache_stage.directory);
	cache_stage.payload = cache_stage.directory = NULL;
	return 1;
}

static void
endwork(void)
{
	if (!end_cache_stage())
		fprintf(stderr,
			"bullet: cannot remove private cache staging: %s\n",
			cache_stage.directory);
	if (asset_temp)
		SDL_RemovePath(asset_temp);
	if (stage)
		SDL_RemovePath(stage);
	if (logpath && !keep_log)
		SDL_RemovePath(logpath);
	if (workdir && !keep_log)
		SDL_RemovePath(workdir);
	free(asset_temp);
	free(stage);
	free(logpath);
	free(workdir);
	asset_temp = stage = logpath = workdir = NULL;
}

static void
cleanup(void)
{
	int status;

	if (child) {
		SDL_KillProcess(child, true);
		SDL_WaitProcess(child, true, &status);
		SDL_DestroyProcess(child);
		child = NULL;
	}
	if (keep_log && logpath)
		fprintf(stderr, "bullet: FFmpeg log: %s\n", logpath);
	endwork();
	freechat();
	free(inferred_chat);
	free(inferred_output);
	surface_free(canvas);
	TTF_CloseFont(font);
	TTF_Quit();
	curl_global_cleanup();
	SDL_Quit();
}

static char *
private_directory(const char *dir)
{
	char *path;
	unsigned int attempt;
	int ok;
#ifdef _WIN32
	wchar_t *w;
#endif

	for (attempt = 0; attempt < 100; attempt++) {
		path = format("%s/.bullet-%llu-%u", dir,
			      (unsigned long long)SDL_GetPerformanceCounter(),
			      attempt);
#ifdef _WIN32
		w = wide(path);
		ok = CreateDirectoryW(w, NULL);
		SDL_free(w);
#else
		ok = mkdir(path, 0700) == 0;
#endif
		if (ok)
			return path;
		free(path);
	}
	die("cannot create private staging directory: %s", dir);
}

static void
beginwork(const char *destination)
{
	char *dir;

	dir = dirnameof(destination);
	check(SDL_CreateDirectory(dir), "create output directory");
	workdir = private_directory(dir);
	free(dir);
	asset_temp = format("%s/asset", workdir);
	logpath = format("%s/ffmpeg.log", workdir);
}

static unsigned char *
readfile(const char *path, size_t limit, size_t *length)
{
	SDL_IOStream *io;
	Sint64 n;
	unsigned char *data;

	io = SDL_IOFromFile(path, "rb");
	check(io != NULL, path);
	n = SDL_GetIOSize(io);
	if (n < 0 || (uint64_t)n > limit)
		die("file exceeds size limit: %s", path);
	data = resize(NULL, (size_t)n + 1, 1);
	if (SDL_ReadIO(io, data, (size_t)n) != (size_t)n)
		die("cannot read %s", path);
	check(SDL_CloseIO(io), "close input");
	data[n] = 0;
	*length = (size_t)n;
	return data;
}

static void
writeall(SDL_IOStream *io, const void *data, size_t n)
{
	const unsigned char *p;
	size_t written;

	p = data;
	while (n) {
		written = SDL_WriteIO(io, p, n);
		if (!written) {
			if (SDL_GetIOStatus(io) != SDL_IO_STATUS_NOT_READY)
				die("write failed: %s", SDL_GetError());
			SDL_Delay(1);
		}
		p += written;
		n -= written;
	}
}

static void
writefile(const char *path, const void *data, size_t n)
{
	SDL_IOStream *io;

	io = SDL_IOFromFile(path, "wb");
	check(io != NULL, path);
	writeall(io, data, n);
	check(SDL_CloseIO(io), "close output");
}

static int
append(Buffer *b, const void *data, size_t n)
{
	size_t cap;

	if (n > b->limit - b->len)
		return 0;
	if (b->len + n + 1 > b->cap) {
		cap = b->len + n + 1;
		if (cap < b->cap * 2)
			cap = b->cap * 2;
		if (cap > b->limit + 1)
			cap = b->limit + 1;
		b->data = resize(b->data, cap, 1);
		b->cap = cap;
	}
	memcpy(b->data + b->len, data, n);
	b->len += n;
	b->data[b->len] = 0;
	return 1;
}

static void
spawn(const char *const *args, int input, int capture, SDL_IOStream *log)
{
	SDL_PropertiesID props;

	props = SDL_CreateProperties();
	check(props != 0, "create process properties");
	check(SDL_SetPointerProperty(
		  props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args),
	      "set args");
	if (input)
		check(SDL_SetNumberProperty(
			  props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
			  SDL_PROCESS_STDIO_APP),
		      "pipe input");
	if (capture)
		check(SDL_SetNumberProperty(
			  props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
			  SDL_PROCESS_STDIO_APP),
		      "pipe output");
	if (log) {
		check(SDL_SetNumberProperty(
			  props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
			  SDL_PROCESS_STDIO_REDIRECT),
		      "redirect stderr");
		check(SDL_SetPointerProperty(
			  props, SDL_PROP_PROCESS_CREATE_STDERR_POINTER, log),
		      "set log");
	}
	child = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (!child)
		die("cannot start %s: %s", args[0], SDL_GetError());
}

static void
waitchild(void)
{
	int status;

	check(SDL_WaitProcess(child, true, &status), "wait for child");
	SDL_DestroyProcess(child);
	child = NULL;
	if (status)
		die("external command failed (exit %d)", status);
}

static char *
capture(const char *const *args)
{
	Buffer b = {NULL, 0, 0, 8 * 1024 * 1024};
	unsigned char block[8192];
	SDL_IOStream *io;
	size_t n;

	spawn(args, 0, 1, NULL);
	io = SDL_GetProcessOutput(child);
	check(io != NULL, "get child output");
	for (;;) {
		n = SDL_ReadIO(io, block, sizeof block);
		if (!append(&b, block, n))
			die("external command output exceeds limit");
		if (!n) {
			if (SDL_GetIOStatus(io) == SDL_IO_STATUS_EOF)
				break;
			if (SDL_GetIOStatus(io) != SDL_IO_STATUS_NOT_READY)
				die("cannot read external command output");
			SDL_Delay(1);
		}
	}
	waitchild();
	return (char *)b.data;
}

static const cJSON *
field(const cJSON *object, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(object, key);
}

static const char *
string(const cJSON *value)
{
	return cJSON_IsString(value) ? value->valuestring : "";
}

static double
number(const cJSON *value)
{
	char *end;
	double n;

	if (cJSON_IsNumber(value)) {
		n = value->valuedouble;
	} else if (cJSON_IsString(value)) {
		errno = 0;
		n = strtod(value->valuestring, &end);
		if (errno || end == value->valuestring || *end)
			die("invalid numeric value");
	} else {
		die("missing numeric value");
	}
	if (!isfinite(n))
		die("non-finite numeric value");
	return n;
}

static int64_t
microseconds(double seconds)
{
	if (seconds < 0 || seconds > (double)MAX_TIME / SECOND)
		die("time is outside 0..7 days");
	return (int64_t)llround(seconds * SECOND);
}

static int64_t
integer(const cJSON *value)
{
	double n;

	n = number(value);
	if (n < 0 || n > 9007199254740991.0 || floor(n) != n)
		die("invalid integer");
	return (int64_t)n;
}

static cJSON *
parsejson(const char *text)
{
	cJSON *root;

	root = cJSON_ParseWithOpts(text, NULL, 1);
	if (!root)
		die("invalid JSON");
	return root;
}

static int64_t
rfc3339(const char *s)
{
	SDL_DateTime dt = {0};
	SDL_Time t;
	int used, hour, minute, offset, digits, fraction;
	const char *p;

	used = 0;
	if (strlen(s) < 20 ||
	    sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &dt.year, &dt.month,
		   &dt.day, &dt.hour, &dt.minute, &dt.second, &used) != 6 ||
	    used != 19 || dt.year < 1970 || dt.year > 2200 || dt.month < 1 ||
	    dt.month > 12 || dt.day < 1 ||
	    dt.day > SDL_GetDaysInMonth(dt.year, dt.month) || dt.hour < 0 ||
	    dt.hour > 23 || dt.minute < 0 || dt.minute > 59 || dt.second < 0 ||
	    dt.second > 59)
		die("invalid RFC3339 timestamp: %s", s);
	p = s + used;
	fraction = digits = 0;
	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			if (digits < 6)
				fraction = fraction * 10 + *p - '0';
			digits++;
			p++;
		}
		if (!digits || digits > 9)
			die("invalid timestamp fraction");
	}
	while (digits++ < 6)
		fraction *= 10;
	offset = 0;
	if (*p == 'Z' && !p[1]) {
		p++;
	} else if ((*p == '+' || *p == '-') && strlen(p) == 6 && p[3] == ':' &&
		   isdigit((unsigned char)p[1]) &&
		   isdigit((unsigned char)p[2]) &&
		   isdigit((unsigned char)p[4]) &&
		   isdigit((unsigned char)p[5])) {
		hour = (p[1] - '0') * 10 + p[2] - '0';
		minute = (p[4] - '0') * 10 + p[5] - '0';
		if (hour > 23 || minute > 59)
			die("invalid UTC offset");
		offset = (hour * 60 + minute) * 60 * (*p == '-' ? -1 : 1);
		p += 6;
	} else {
		die("timestamp requires a UTC offset");
	}
	if (*p)
		die("invalid timestamp suffix");
	check(SDL_DateTimeToTime(&dt, &t), "convert timestamp");
	return t / 1000 + fraction - (int64_t)offset * SECOND;
}

static void
rate(const char *s, int *num, int *den)
{
	long a, b;
	char *end;

	errno = 0;
	a = strtol(s, &end, 10);
	b = 1;
	if (end == s)
		die("invalid frame rate");
	if (*end == '/') {
		s = end + 1;
		b = strtol(s, &end, 10);
		if (end == s)
			die("invalid frame rate denominator");
	}
	if (errno || *end || a < 1 || a > INT_MAX || b < 1 || b > INT_MAX ||
	    (double)a / b > 240)
		die("invalid frame rate: expected 0 < fps <= 240");
	*num = (int)a;
	*den = (int)b;
}

static int64_t
frame_time(int64_t frame, int num, int den)
{
	int64_t ticks;

	if (frame < 0 || frame > INT64_MAX / den)
		die("frame counter overflow");
	ticks = frame * den;
	if (ticks / num > INT64_MAX / SECOND - 1)
		die("frame time overflow");
	return (ticks / num) * SECOND + (ticks % num) * SECOND / num;
}

/* First frame whose integer microsecond clock reaches time. Split the
 * product so large, valid rational frame rates do not overflow. */
static int64_t
frame_ceiling(int64_t time, int num, int den)
{
	int64_t ticks, rest, divisor;

	if (time <= 0)
		return 0;
	if (time > MAX_TIME)
		die("frame time exceeds limit");
	ticks = (time / SECOND) * num;
	rest = (ticks % den) * SECOND + (time % SECOND) * num;
	divisor = (int64_t)den * SECOND;
	return ticks / den + rest / divisor + (rest % divisor != 0);
}

static cJSON *
probejson(const char *path)
{
	const char *args[] = {"ffprobe",      "-v",	  "error",
			      "-max_alloc",   "67108864", "-show_streams",
			      "-show_format", "-of",	  "json",
			      path,	      NULL};
	char *text;
	cJSON *root;

	text = capture(args);
	root = parsejson(text);
	free(text);
	return root;
}

static const cJSON *
video_stream(const cJSON *root)
{
	const cJSON *s;

	cJSON_ArrayForEach (s, field(root, "streams")) {
		if (!strcmp(string(field(s, "codec_type")), "video"))
			return s;
	}
	die("no video stream");
}

static Video
probe(const char *path)
{
	cJSON *root;
	const cJSON *s, *entry;
	const char *sar, *fps;
	Video v;
	double w, h, a, b, angle;

	root = probejson(path);
	s = video_stream(root);
	w = number(field(s, "width"));
	h = number(field(s, "height"));
	sar = string(field(s, "sample_aspect_ratio"));
	if (*sar && strcmp(sar, "N/A") && strcmp(sar, "0:1")) {
		if (sscanf(sar, "%lf:%lf", &a, &b) != 2 || !isfinite(a) ||
		    !isfinite(b) || a <= 0 || b <= 0)
			die("invalid sample aspect ratio");
		w = round(w * a / b);
	}
	angle = 0;
	entry = field(field(s, "tags"), "rotate");
	if (entry)
		angle = number(entry);
	cJSON_ArrayForEach (entry, field(s, "side_data_list")) {
		if (field(entry, "rotation"))
			angle = number(field(entry, "rotation"));
	}
	if (fabs(remainder(angle, 180.0)) > 0.01) {
		a = w;
		w = h;
		h = a;
	}
	if (w < 2 || h < 2 || w > 16000 || h > 16000 || w * h > MAX_PIXELS)
		die("video dimensions exceed limit");
	v.width = (int)w;
	v.height = (int)h;
	v.duration =
	    microseconds(number(field(field(root, "format"), "duration")));
	if (!v.duration)
		die("empty video");
	fps = string(field(s, "avg_frame_rate"));
	if (!*fps || !strcmp(fps, "0/0"))
		fps = string(field(s, "r_frame_rate"));
	rate(fps, &v.fps_num, &v.fps_den);
	cJSON_Delete(root);
	return v;
}

static size_t
asset(const char *url, double aspect)
{
	size_t i;

	if (strncmp(url, "https://", 8) || strlen(url) > 8192)
		die("invalid HTTPS emote URL");
	for (i = 0; i < nassets; i++)
		if (!strcmp(url, assets[i].url))
			return i;
	if (nassets == MAX_ASSETS || !isfinite(aspect) || aspect <= 0 ||
	    aspect > 256)
		die("emote count or aspect ratio exceeds limit");
	assets = resize(assets, nassets + 1, sizeof *assets);
	memset(&assets[nassets], 0, sizeof *assets);
	assets[nassets].url = copystr(url);
	assets[nassets].aspect = aspect;
	return nassets++;
}

static double
aspectof(const cJSON *item)
{
	const cJSON *w, *h;
	double width, height;

	w = field(item, "width");
	h = field(item, "height");
	width = w ? number(w) : 0;
	height = h ? number(h) : 0;
	return width > 0 && height > 0 ? width / height : 1;
}

static char *
twitch_url(const cJSON *id)
{
	const char *s;
	char numeric[32];
	size_t i;

	if (cJSON_IsString(id)) {
		s = string(id);
	} else {
		snprintf(numeric, sizeof numeric, "%lld",
			 (long long)integer(id));
		s = numeric;
	}
	if (!*s || strlen(s) > 256)
		die("invalid emote id");
	for (i = 0; s[i]; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '_' &&
		    s[i] != '-')
			die("invalid emote id");
	return format("https://static-cdn.jtvnw.net/emoticons/v2/%s/"
		      "default/dark/2.0",
		      s);
}

static Message *
message(int64_t time)
{
	Message *m;

	if (time < 0 || time > MAX_TIME || nmessages == MAX_MESSAGES)
		die("message count or time exceeds limit");
	messages = resize(messages, nmessages + 1, sizeof *messages);
	m = &messages[nmessages];
	memset(m, 0, sizeof *m);
	m->time = time;
	m->stamp = -1;
	m->order = nmessages++;
	return m;
}

static void
part(Message *m, const char *text, size_t image)
{
	Part *p;
	char *s;

	if (!*text && image == NONE)
		return;
	if (strlen(text) > 32768 || ++nparts > MAX_PARTS)
		die("comment text or part count exceeds limit");
	m->parts = resize(m->parts, m->count + 1, sizeof *m->parts);
	p = &m->parts[m->count++];
	memset(p, 0, sizeof *p);
	p->asset = image;
	p->text = copystr(text);
	for (s = p->text; *s; s++)
		if (*s == '\r' || *s == '\n')
			*s = ' ';
}

static void
read_twitch(const cJSON *root)
{
	const cJSON *item, *comments, *body, *fragments, *f, *id;
	char *url;
	size_t index;
	Message *m;
	int64_t time;

	comments = field(root, "comments");
	if (!cJSON_IsArray(comments))
		die("missing Twitch comments array");
	cJSON_ArrayForEach (item,
			    field(field(root, "embeddedData"), "firstParty")) {
		url = twitch_url(field(item, "id"));
		index = asset(url, aspectof(item));
		free(url);
		free(assets[index].embedded);
		assets[index].embedded = copystr(string(field(item, "data")));
	}
	cJSON_ArrayForEach (item, comments) {
		time = microseconds(
		    number(field(item, "content_offset_seconds")));
		if (opt.hls) {
			time = rfc3339(string(field(item, "created_at"))) -
			       opt.origin;
			if (time < 0)
				die("comment precedes HLS start");
		}
		m = message(time);
		body = field(item, "message");
		if (!cJSON_IsObject(body))
			die("missing Twitch message");
		fragments = field(body, "fragments");
		if (!cJSON_IsArray(fragments) || !fragments->child) {
			part(m, string(field(body, "body")), NONE);
			continue;
		}
		cJSON_ArrayForEach (f, fragments) {
			id = field(field(f, "emoticon"), "emoticon_id");
			if (id && !cJSON_IsNull(id)) {
				url = twitch_url(id);
				index = asset(url, 1);
				free(url);
				part(m, "", index);
			} else {
				part(m, string(field(f, "text")), NONE);
			}
		}
	}
}

static void
read_youtube(const cJSON *root)
{
	const cJSON *replay, *a, *item, *body, *runs, *r, *emoji, *thumb;
	const cJSON *best;
	Message *m;
	int64_t time;
	double width, best_width;

	if (opt.hls)
		die("--hls-start is only supported for Twitch chat");
	replay = field(root, "replayChatItemAction");
	if (!cJSON_IsObject(replay))
		die("missing replayChatItemAction");
	time = integer(field(replay, "videoOffsetTimeMsec"));
	if (time > MAX_TIME / 1000)
		die("replay time exceeds limit");
	cJSON_ArrayForEach (a, field(replay, "actions")) {
		cJSON_ArrayForEach (
		    item, field(field(a, "addChatItemAction"), "item")) {
			if (!item->string ||
			    (strcmp(item->string,
				    "liveChatTextMessageRenderer") &&
			     strcmp(item->string,
				    "liveChatPaidMessageRenderer") &&
			     strcmp(item->string,
				    "liveChatMembershipItemRenderer")))
				continue;
			m = message(time * 1000);
			if (field(item, "timestampUsec"))
				m->stamp =
				    integer(field(item, "timestampUsec"));
			body = field(item, "message");
			runs = field(body, "runs");
			if (!cJSON_IsArray(runs) || !runs->child)
				part(m, string(field(body, "simpleText")),
				     NONE);
			cJSON_ArrayForEach (r, runs) {
				if (cJSON_IsString(field(r, "text"))) {
					part(m, string(field(r, "text")),
					     NONE);
					continue;
				}
				emoji = field(r, "emoji");
				if (!emoji)
					continue;
				if (!cJSON_IsTrue(
					field(emoji, "isCustomEmoji"))) {
					part(m,
					     string(field(emoji, "emojiId")),
					     NONE);
					continue;
				}
				best = NULL;
				best_width = -1;
				cJSON_ArrayForEach (
				    thumb, field(field(emoji, "image"),
						 "thumbnails")) {
					if (!*string(field(thumb, "url")))
						continue;
					width =
					    field(thumb, "width")
						? number(field(thumb, "width"))
						: 0;
					if (width >= best_width) {
						best = thumb;
						best_width = width;
					}
				}
				if (!best)
					die("custom emoji has no image URL");
				part(m, "",
				     asset(string(field(best, "url")),
					   aspectof(best)));
			}
		}
	}
}

static int
compare_time(const void *a, const void *b)
{
	const Message *x, *y;

	x = a;
	y = b;
	if (x->time != y->time)
		return x->time < y->time ? -1 : 1;
	return (x->order > y->order) - (x->order < y->order);
}

static int
compare_integer(const void *a, const void *b)
{
	int64_t x, y;

	x = *(const int64_t *)a;
	y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static void
readchat(const char *path)
{
	unsigned char *data;
	const char *p, *end;
	cJSON *root;
	size_t length, i, j, count, skipped;
	int twitch;
	int64_t *starts, start;

	chat_context = path;
	data = readfile(path, MAX_JSON, &length);
	if (memchr(data, 0, length))
		die("NUL byte in JSON input");
	p = (const char *)data;
	root = cJSON_ParseWithLengthOpts(p, length + 1, &end, 0);
	if (!root)
		die("invalid chat JSON");
	twitch = field(root, "comments") != NULL;
	for (;;) {
		if (twitch)
			read_twitch(root);
		else
			read_youtube(root);
		cJSON_Delete(root);
		p = end;
		while (isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (twitch)
			die("trailing data after Twitch JSON");
		root = cJSON_ParseWithLengthOpts(
		    p, length + 1 - (size_t)(p - (const char *)data), &end, 0);
		if (!root)
			die("invalid YouTube replay JSON");
	}
	free(data);
	starts = NULL;
	count = skipped = 0;
	if (!twitch) {
		starts = resize(NULL, nmessages + 1, sizeof *starts);
		for (i = 0; i < nmessages; i++)
			if (messages[i].time && messages[i].stamp >= 0 &&
			    messages[i].count)
				starts[count++] =
				    messages[i].stamp - messages[i].time;
	}
	start = 0;
	if (count) {
		qsort(starts, count, sizeof *starts, compare_integer);
		start = starts[count / 2];
		if (!(count % 2))
			start = starts[count / 2 - 1] +
				(start - starts[count / 2 - 1]) / 2;
	}
	free(starts);
	j = 0;
	for (i = 0; i < nmessages; i++) {
		if (!messages[i].count ||
		    (count && !messages[i].time &&
		     (messages[i].stamp < 0 || messages[i].stamp < start))) {
			if (messages[i].count)
				skipped++;
			for (length = 0; length < messages[i].count; length++)
				free(messages[i].parts[length].text);
			free(messages[i].parts);
		} else {
			messages[j++] = messages[i];
		}
	}
	nmessages = j;
	if (!nmessages)
		die("no text or emoji messages found in replay chat");
	qsort(messages, nmessages, sizeof *messages, compare_time);
	if (skipped)
		fprintf(stderr, "bullet: skipped %zu pre-stream comments\n",
			skipped);
	chat_context = NULL;
}

static void
hashurl(const char *url, char hex[65])
{
	unsigned char digest[32];
	size_t i;
#ifdef _WIN32
	BCRYPT_ALG_HANDLE algorithm;

	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
					NULL, 0) < 0)
		die("cannot open SHA-256 provider");
	if (BCryptHash(algorithm, NULL, 0, (PUCHAR)url, (ULONG)strlen(url),
		       digest, sizeof digest) < 0)
		die("cannot hash emote URL");
	BCryptCloseAlgorithmProvider(algorithm, 0);
#else
	if (!SHA256((const unsigned char *)url, strlen(url), digest))
		die("cannot hash emote URL");
#endif
	for (i = 0; i < sizeof digest; i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
}

static unsigned char *
unbase64(const char *s, size_t *length)
{
	unsigned char *bytes;
	size_t n;
#ifdef _WIN32
	DWORD size;
#else
	int decoded;
#endif

	n = strlen(s);
	if (n > MAX_ASSET * 4 / 3 + 4)
		die("embedded emote exceeds 8 MiB");
	bytes = resize(NULL, n + 1, 1);
#ifdef _WIN32
	size = (DWORD)n;
	if (!CryptStringToBinaryA(s, (DWORD)n,
				  CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
				  bytes, &size, NULL, NULL))
		die("invalid embedded base64 image");
	*length = size;
#else
	decoded = EVP_DecodeBlock(bytes, (const unsigned char *)s, (int)n);
	if (decoded < 0 || n % 4)
		die("invalid embedded base64 image");
	if (n && s[n - 1] == '=')
		decoded--;
	if (n > 1 && s[n - 2] == '=')
		decoded--;
	if (decoded < 0)
		die("invalid embedded base64 image");
	*length = (size_t)decoded;
#endif
	if (*length > MAX_ASSET)
		die("embedded emote exceeds 8 MiB");
	return bytes;
}

static size_t
receive(char *data, size_t size, size_t count, void *context)
{
	Buffer *b;
	size_t n;

	b = context;
	if (size && count > SIZE_MAX / size)
		return 0;
	n = size * count;
	return append(b, data, n) ? n : 0;
}

static unsigned char *
fetch(const char *url, size_t *length)
{
	Buffer b = {NULL, 0, 0, MAX_ASSET};
	CURL *curl;
	CURLcode result;

	curl = curl_easy_init();
	if (!curl)
		die("cannot initialize HTTP client");
	if (curl_easy_setopt(curl, CURLOPT_URL, url) ||
	    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") ||
	    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") ||
	    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) ||
	    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L) ||
	    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L) ||
	    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L) ||
	    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 bullet") ||
	    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive) ||
	    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b))
		die("cannot configure HTTP client");
	result = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (result != CURLE_OK)
		die("fetching %s: %s (image limit: 8 MiB)", url,
		    curl_easy_strerror(result));
	*length = b.len;
	return b.data;
}

static void
asset_frame(Asset *a, SDL_Surface *source, uint64_t delay_ms)
{
	SDL_Surface *rgba, *out;
	int width;
	int64_t end;

	if (a->count == MAX_FRAMES || delay_ms > 86400000)
		die("emote frame count or duration exceeds limit");
	width = (int)round((double)emote_height * source->w / source->h);
	if (width < 1)
		width = 1;
	surface_limit(width, emote_height);
	rgba = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
	check(rgba != NULL, "convert emote pixels");
	/* Own the resized pixels directly; no second surface and row copy. */
	out = rgba;
	if (rgba->w != width || rgba->h != emote_height) {
		out = SDL_ScaleSurface(rgba, width, emote_height,
				       SDL_SCALEMODE_LINEAR);
		check(out != NULL, "resize emote");
		SDL_DestroySurface(rgba);
	}
	surface_bytes += (size_t)out->pitch * out->h;
	a->frames = resize(a->frames, a->count + 1, sizeof *a->frames);
	a->ends = resize(a->ends, a->count + 1, sizeof *a->ends);
	end = a->count ? a->ends[a->count - 1] : 0;
	a->frames[a->count] = out;
	a->ends[a->count++] =
	    end + (int64_t)(delay_ms < 20 ? 20 : delay_ms) * 1000;
}

static void
load_asset(Asset *a, const char *directory)
{
	char hex[65], *path;
	unsigned char *bytes;
	size_t length;
	int cached, isgif;
	cJSON *metadata;
	const cJSON *stream;
	double width, height;
	SDL_IOStream *io;
	SDL_Surface *frame;
	IMG_AnimationDecoder *decoder;
	Uint64 delay;
	const char *verify[] = {
	    "ffmpeg",	"-v",		"error", "-xerror",   "-max_alloc",
	    "67108864", "-ignore_loop", "1",	 "-i",	      asset_temp,
	    "-an",	"-frames:v",	"501",	 "-fps_mode", "passthrough",
	    "-f",	"null",		"-",	 NULL};

	hashurl(a->url, hex);
	path = format("%s/%s", directory, hex);
	cached = exists(path);
	if (cached)
		bytes = readfile(path, MAX_ASSET, &length);
	else if (a->embedded && *a->embedded)
		bytes = unbase64(a->embedded, &length);
	else
		bytes = fetch(a->url, &length);
	if (!length)
		die("empty emote: %s", a->url);
	/* Probe dimensions before allowing a codec to expand untrusted data.
	 */
	writefile(asset_temp, bytes, length);
	metadata = probejson(asset_temp);
	stream = video_stream(metadata);
	width = number(field(stream, "width"));
	height = number(field(stream, "height"));
	if (width < 1 || height < 1 || width > 4096 || height > 4096)
		die("emote exceeds 4096x4096: %s", a->url);
	cJSON_Delete(metadata);
	io = SDL_IOFromConstMem(bytes, length);
	check(io != NULL, "open emote bytes");
	isgif = IMG_isGIF(io);
	if (isgif) {
		/* SDL_image accepts some truncated GIFs as an ordinary EOF. */
		spawn(verify, 0, 0, NULL);
		waitchild();
		decoder = IMG_CreateAnimationDecoder_IO(io, true, "GIF");
		check(decoder != NULL, "open GIF decoder");
		while (IMG_GetAnimationDecoderFrame(decoder, &frame, &delay)) {
			if (frame->w != (int)width || frame->h != (int)height)
				die("GIF canvas changed dimensions");
			asset_frame(a, frame, delay);
			SDL_DestroySurface(frame);
		}
		if (IMG_GetAnimationDecoderStatus(decoder) !=
		    IMG_DECODER_STATUS_COMPLETE)
			die("invalid GIF: %s", SDL_GetError());
		check(IMG_CloseAnimationDecoder(decoder), "close GIF decoder");
	} else {
		if (!IMG_isPNG(io) && !IMG_isJPG(io) && !IMG_isWEBP(io))
			die("unsupported emote format: %s", a->url);
		frame = IMG_Load_IO(io, true);
		check(frame != NULL, "decode emote");
		if (frame->w != (int)width || frame->h != (int)height)
			die("emote dimensions disagree with metadata");
		asset_frame(a, frame, 100);
		SDL_DestroySurface(frame);
	}
	if (!a->count)
		die("emote has no frames: %s", a->url);
	if (!cached) {
		if (cache_stage.directory || cache_stage.payload)
			die("private cache stage is already active");
		cache_stage.directory = private_directory(directory);
		cache_stage.payload =
		    format("%s/asset", cache_stage.directory);
		writefile(cache_stage.payload, bytes, length);
		if (!commitfile(cache_stage.payload, path, 0))
			die("cannot commit emote cache without overwriting: "
			    "%s",
			    path);
		free(cache_stage.payload);
		cache_stage.payload = NULL;
		check(end_cache_stage(), "remove private cache staging");
	}
	free(bytes);
	SDL_RemovePath(asset_temp);
	free(path);
}

static SDL_Surface *
frame_at(const Asset *a, int64_t elapsed)
{
	size_t first, last, mid;
	int64_t time;

	time = elapsed % a->ends[a->count - 1];
	first = 0;
	last = a->count - 1;
	while (first < last) {
		mid = first + (last - first) / 2;
		if (time < a->ends[mid])
			last = mid;
		else
			first = mid + 1;
	}
	return a->frames[first];
}

/* Straight-alpha source-over, including colour in translucent pixels. */
static void
paste_pixels(unsigned char *d, const unsigned char *s, int count)
{
	int i, c;
	unsigned int sa, da, inv, out, value;

	for (i = 0; i < count; i++, s += 4, d += 4) {
		sa = s[3];
		if (!sa)
			continue;
		da = d[3];
		/* These cases are exactly a copy, including RGB under alpha.
		 */
		if (sa == 255 || !da) {
			memcpy(d, s, 4);
			continue;
		}
		inv = 255 - sa;
		out = sa + (da * inv + 127) / 255;
		for (c = 0; c < 3; c++) {
			value = s[c] * sa * 255 + d[c] * da * inv;
			value = (value + out * 255 / 2) / (out * 255);
			d[c] = (unsigned char)(value > 255 ? 255 : value);
		}
		d[3] = (unsigned char)out;
	}
}

/* Keep 16 fractional bits while retaining the integer-position fast path. */
static unsigned int
subpixel_position(double x, int *pixel)
{
	unsigned int fraction;

	*pixel = (int)floor(x);
	fraction = (unsigned int)round((x - *pixel) * SUBPIXEL_SCALE);
	if (fraction == SUBPIXEL_SCALE) {
		(*pixel)++;
		fraction = 0;
	}
	return fraction;
}

static void
paste_row(SDL_Surface *to, const unsigned char *pixels, int count, int x,
	  int y, unsigned int fraction)
{
	const unsigned char clear[4] = {0};
	const unsigned char *a, *b;
	unsigned char *d, sample[4];
	unsigned int aw, bw, alpha, value;
	int i, c, left, right, end;

	end = count + (fraction != 0);
	left = x < 0 ? -x : 0;
	right = end < to->w - x ? end : to->w - x;
	if (left >= right)
		return;
	d = (unsigned char *)to->pixels + y * to->pitch + (x + left) * 4;
	if (!fraction) {
		paste_pixels(d, pixels + left * 4, right - left);
		return;
	}
	/* Interpolate premultiplied colour, then source-over only once.
	 * Transparent RGB must not bleed into text or emote edges. */
	for (i = left; i < right; i++, d += 4) {
		a = i ? pixels + (i - 1) * 4 : clear;
		b = i < count ? pixels + i * 4 : clear;
		aw = a[3] * fraction;
		bw = b[3] * (SUBPIXEL_SCALE - fraction);
		alpha = aw + bw;
		if (!alpha)
			continue;
		for (c = 0; c < 3; c++) {
			value = a[c] * aw + b[c] * bw;
			sample[c] =
			    (unsigned char)((value + alpha / 2) / alpha);
		}
		sample[3] = (unsigned char)((alpha + SUBPIXEL_SCALE / 2) /
					    SUBPIXEL_SCALE);
		paste_pixels(d, sample, 1);
	}
}

static void
paste(SDL_Surface *to, const SDL_Surface *from, double x, int y)
{
	int pixel, sy, top, bottom;
	unsigned int fraction;
	const unsigned char *s;

	if (x >= to->w || x + from->w <= 0)
		return;
	fraction = subpixel_position(x, &pixel);
	top = y < 0 ? -y : 0;
	bottom = from->h < to->h - y ? from->h : to->h - y;
	for (sy = top; sy < bottom; sy++) {
		s = (const unsigned char *)from->pixels + sy * from->pitch;
		paste_row(to, s, from->w, pixel, y + sy, fraction);
	}
}

static void
paste_sprite(SDL_Surface *to, const Sprite *s, double x, int y)
{
	Run run;
	size_t offset;
	int pixel, dy;
	unsigned int fraction;
	const unsigned char *pixels;

	if (s->pixels) {
		paste(to, s->pixels, x, y);
		return;
	}
	fraction = subpixel_position(x, &pixel);
	for (offset = 0; offset < s->bytes;) {
		memcpy(&run, s->runs + offset, sizeof run);
		pixels = s->runs + offset + sizeof run;
		offset += sizeof run + (size_t)run.width * 4;
		dy = y + run.y;
		if (dy < 0)
			continue;
		if (dy >= to->h)
			break;
		paste_row(to, pixels, run.width, pixel + run.x, dy, fraction);
	}
}

static void
openfont(int width, int height)
{
	const char *path;
	size_t i;

	font_size = (int)round((double)(width < height ? width : height) / 22);
	if (font_size < 16)
		font_size = 16;
	outline = (int)round(font_size * 1.2 / 28);
	if (outline < 1)
		outline = 1;
	gap = (int)round(font_size * 5.0 / 28);
	if (gap < 2)
		gap = 2;
	lane_height = (int)ceil(font_size * 1.5) + 2 * outline;
	emote_height = lane_height - gap;
	path = opt.font;
	for (i = 0; !path && i < sizeof fonts / sizeof *fonts; i++)
		if (exists(fonts[i]))
			path = fonts[i];
	if (!path)
		die("no usable font; specify --font");
	check(TTF_Init(), "initialize font library");
	font = TTF_OpenFont(path, (float)font_size);
	check(font != NULL, "open font");
}

static size_t
layout(int width, int height)
{
	size_t i, j, k, lanes, row, choice, fallbacks, count, best_count;
	size_t *heads;
	Message *m, *old;
	Part *p;
	int text_height, position, safe;
	double speed, old_speed, right, furthest, best, clearance;

	lanes = (size_t)(height / lane_height);
	if (!lanes)
		lanes = 1;
	heads = resize(NULL, lanes, sizeof *heads);
	for (i = 0; i < lanes; i++)
		heads[i] = NONE;
	fallbacks = 0;
	clearance = gap > font_size / 2 ? gap : font_size / 2;
	for (i = 0; i < nmessages; i++) {
		m = &messages[i];
		position = 0;
		for (j = 0; j < m->count; j++) {
			p = &m->parts[j];
			p->x = position;
			if (p->asset == NONE) {
				check(TTF_GetStringSize(font, p->text, 0,
							&p->width,
							&text_height),
				      "measure text");
				p->width += 2 * outline;
			} else {
				p->width = (int)round(emote_height *
						      assets[p->asset].aspect);
			}
			if (p->width < 0 || p->width > 65530 - position - gap)
				die("comment is too wide");
			position += p->width + (j + 1 < m->count ? gap : 0);
		}
		m->width = position;
		speed = (width + (double)m->width) / opt.travel;
		choice = NONE;
		best = HUGE_VAL;
		best_count = SIZE_MAX;
		for (row = 0; row < lanes; row++) {
			safe = 1;
			furthest = -HUGE_VAL;
			count = 0;
			for (k = heads[row]; k != NONE; k = old->next) {
				old = &messages[k];
				if (old->time + opt.travel <= m->time)
					break;
				count++;
				old_speed =
				    (width + (double)old->width) / opt.travel;
				right = width -
					old_speed * (m->time - old->time) +
					old->width;
				if (right > furthest)
					furthest = right;
				if (right > width - clearance ||
				    (old_speed < speed &&
				     width - speed * (old->time + opt.travel -
						      m->time) <
					 clearance))
					safe = 0;
			}
			if (safe) {
				choice = row;
				break;
			}
			if (furthest < best ||
			    (furthest == best && count < best_count)) {
				choice = row;
				best = furthest;
				best_count = count;
			}
		}
		if (row == lanes)
			fallbacks++;
		m->y = (int)choice * lane_height;
		m->next = heads[choice];
		heads[choice] = i;
	}
	free(heads);
	return fallbacks;
}

static SDL_Surface *
text_surface(const char *text, SDL_Color color)
{
	SDL_Surface *s, *rgba;

	s = TTF_RenderText_Blended(font, text, 0, color);
	check(s != NULL, "rasterize text");
	rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(s);
	check(rgba != NULL, "convert text pixels");
	return rgba;
}

static void
bake(Message *m)
{
	SDL_Color white = {255, 255, 255, 255}, black = {0, 0, 0, 255};
	SDL_Surface *fill, *edge, *image, *pixels;
	Part *p;
	size_t j;
	int y, dx, dy;

	pixels = surface(m->width + 2, lane_height);
	for (j = 0; j < m->count; j++) {
		p = &m->parts[j];
		if (p->asset != NONE) {
			if (assets[p->asset].count == 1) {
				image = assets[p->asset].frames[0];
				paste(pixels, image, p->x,
				      (lane_height - image->h) / 2);
			}
			continue;
		}
		fill = text_surface(p->text, white);
		y = (lane_height - fill->h) / 2;
		if (opt.shadow) {
			edge = text_surface(p->text, black);
			for (dy = -2; dy <= 2; dy += 4)
				for (dx = -2; dx <= 2; dx += 4)
					paste(pixels, edge,
					      p->x + outline + dx, y + dy);
		} else {
			check(TTF_SetFontOutline(font, outline),
			      "set outline");
			edge = text_surface(p->text, black);
			check(TTF_SetFontOutline(font, 0), "reset outline");
			paste(pixels, edge, p->x, y - outline);
		}
		paste(pixels, fill, p->x + outline, y);
		SDL_DestroySurface(fill);
		SDL_DestroySurface(edge);
	}
	m->sprite = sprite_create(pixels);
}

static void
drawframe(int64_t now, size_t first, size_t last, int top)
{
	Message *m;
	Part *p;
	SDL_Surface *image;
	size_t i, j;
	int row, column;
	double x;
	unsigned char *pixels;

	memset(canvas->pixels, 0, (size_t)canvas->pitch * canvas->h);
	for (i = first; i < last; i++) {
		m = &messages[i];
		x = canvas->w - (canvas->w + (double)m->width) *
				    (now - m->time) / opt.travel;
		if (x >= canvas->w || x + m->width <= 0)
			continue;
		if (!m->sprite)
			bake(m);
		paste_sprite(canvas, m->sprite, x, m->y - top);
		for (j = 0; j < m->count; j++) {
			p = &m->parts[j];
			if (p->asset == NONE || assets[p->asset].count == 1)
				continue;
			image = frame_at(&assets[p->asset], now - m->time);
			paste(canvas, image, x + p->x,
			      m->y - top + (lane_height - image->h) / 2);
		}
	}
	if (opt.opacity == 100)
		return;
	for (row = 0; row < canvas->h; row++) {
		pixels = (unsigned char *)canvas->pixels + row * canvas->pitch;
		for (column = 0; column < canvas->w; column++)
			pixels[column * 4 + 3] =
			    (unsigned char)((pixels[column * 4 + 3] *
						 opt.opacity +
					     50) /
					    100);
	}
}

static Overlay
overlay_plan(int height)
{
	Overlay o = {0};
	int bottom;
	int64_t begin, end, stop;
	size_t i, j;
	Message *m;

	stop = opt.start + opt.duration;
	begin = stop;
	end = opt.start;
	o.y = height;
	bottom = 0;
	for (i = 0; i < nmessages; i++) {
		m = &messages[i];
		if (m->time >= stop || m->time + opt.travel <= opt.start)
			continue;
		o.visible++;
		if (m->time < begin)
			begin = m->time;
		if (m->time + opt.travel > end)
			end = m->time + opt.travel;
		if (m->y < o.y)
			o.y = m->y;
		if (m->y + lane_height > bottom)
			bottom = m->y + lane_height;
		for (j = 0; j < m->count; j++)
			if (m->parts[j].asset != NONE)
				assets[m->parts[j].asset].needed = 1;
	}
	if (end > stop)
		end = stop;
	o.first = frame_ceiling(begin - opt.start, opt.fps_num, opt.fps_den);
	o.end = frame_ceiling(end - opt.start, opt.fps_num, opt.fps_den);
	o.height = (bottom < height ? bottom : height) - o.y;
	if (o.first >= o.end) {
		/* A single transparent frame keeps the same RGB filter
		 * negotiation even when no chat is sampled in this clip. */
		o.y = 0;
		o.height = 1;
		o.first = 0;
		o.end = 1;
	}
	return o;
}

static void
check_names(void)
{
	const char *video, *chat, *dot;
	size_t n, length;

	video = basenameof(opt.video);
	chat = basenameof(opt.chat);
	dot = strrchr(video, '.');
	n = dot ? (size_t)(dot - video) : strlen(video);
	length = strlen(chat);
	if (length >= 15 && !strcmp(chat + length - 15, ".live_chat.json"))
		length -= 15;
	else if (length >= 10 && !strcmp(chat + length - 10, ".chat.json"))
		length -= 10;
	else
		return;
	if (length != n || strncmp(video, chat, n))
		die("video/chat IDs do not match");
}

static void
render(void)
{
	Video v;
	Overlay overlay;
	char fps[32], dimensions[32], start[32], duration[32];
	char filter[512], *directory, *parent;
	const char *extension;
	const char *args[] = {"ffmpeg",
			      "-hide_banner",
			      "-loglevel",
			      "error",
			      "-xerror",
			      "-y",
			      "-ss",
			      start,
			      "-i",
			      opt.video,
			      "-f",
			      "rawvideo",
			      "-pixel_format",
			      "rgba",
			      "-video_size",
			      dimensions,
			      "-framerate",
			      fps,
			      "-i",
			      "pipe:0",
			      "-filter_complex",
			      filter,
			      "-map",
			      "[v]",
			      "-map",
			      "0:a?",
			      "-t",
			      duration,
			      "-c:v",
			      "libx264",
			      "-preset",
			      "medium",
			      "-crf",
			      "16",
			      "-pix_fmt",
			      "yuv420p",
			      "-c:a",
			      "copy",
			      "-movflags",
			      "+faststart",
			      NULL,
			      NULL};
	SDL_IOStream *log, *input;
	int width, height, row, blank;
	int64_t now, n;
	Uint64 started, reported, ticks;
	double elapsed;
	size_t i, first, last, fallbacks;

	started = reported = SDL_GetTicks();
	fprintf(stderr, "bullet: video: %s\n", opt.video);
	fprintf(stderr, "bullet: chat: %s\n", opt.chat);
	fprintf(stderr, "bullet: output: %s\n", opt.output);
	check_destination();
	check_names();
	v = probe(opt.video);
	if (opt.start >= v.duration)
		die("--start is past the video end");
	if (opt.duration < 0 || opt.duration > v.duration - opt.start)
		opt.duration = v.duration - opt.start;
	height = opt.height ? opt.height : v.height;
	width = opt.height
		    ? (int)(2 * round((double)v.width * height / v.height / 2))
		    : v.width;
	if (width < 2 || width > 16000 || height < 2 || height > 16000 ||
	    width % 2 || height % 2 || (int64_t)width * height > MAX_PIXELS)
		die("output dimensions must be even and within the pixel "
		    "limit");
	if (!opt.fps_num) {
		opt.fps_num = v.fps_num;
		opt.fps_den = v.fps_den;
	}
	fprintf(stderr, "bullet: reading chat\n");
	readchat(opt.chat);
	fprintf(stderr, "bullet: laying out %zu messages\n", nmessages);
	openfont(width, height);
	fallbacks = layout(width, height);
	beginwork(opt.output);
	extension = strrchr(basenameof(opt.output), '.');
	if (!extension || !extension[1])
		die("output must have a video extension");
	stage = format("%s/video%s", workdir, extension);
	parent = dirnameof(opt.chat);
	directory = format("%s/assets", parent);
	free(parent);
	overlay = overlay_plan(height);
#ifdef BULLET_TEST
	/* Test-only oracle: the original full-frame, full-duration stream. */
	if (dense_reference) {
		overlay.y = 0;
		overlay.height = height;
		overlay.first = 0;
		overlay.end =
		    frame_ceiling(opt.duration, opt.fps_num, opt.fps_den);
	}
#endif
	fprintf(stderr, "bullet: preparing visible images and GIFs\n");
	for (i = 0; i < nassets; i++) {
		if (assets[i].needed) {
			check(SDL_CreateDirectory(directory),
			      "create asset cache");
			load_asset(&assets[i], directory);
		}
	}
	free(directory);
	canvas = surface(width, overlay.height);
	snprintf(fps, sizeof fps, "%d/%d", opt.fps_num, opt.fps_den);
	snprintf(dimensions, sizeof dimensions, "%dx%d", width,
		 overlay.height);
	snprintf(start, sizeof start, "%.6f", (double)opt.start / SECOND);
	snprintf(duration, sizeof duration, "%.6f",
		 (double)opt.duration / SECOND);
	/* Even transparent chat used the YUV -> RGB -> YUV round trip.
	 * Keep that negotiation, not a YUV overlay or a direct base bypass.
	 * Rawvideo PTS units are whole CFR frames, so the offset is exact. */
	snprintf(filter, sizeof filter,
		 "[0:v]fps=%s,scale=%d:%d:flags=lanczos,setsar=1,"
		 "format=yuv420p[base];[1:v]setpts=PTS+%lld[chat];"
		 "[base][chat]overlay=0:%d:format=auto:"
		 "eof_action=pass:repeatlast=0,format=yuv420p[v]",
		 fps, width, height, (long long)overlay.first, overlay.y);
	args[sizeof args / sizeof *args - 2] = stage;
	/* Seeking at zero discards AAC priming packets; do not seek a full
	 * VOD. */
	if (!opt.start)
		memmove(args + 6, args + 8, sizeof args - 8 * sizeof *args);
	log = SDL_IOFromFile(logpath, "wb");
	check(log != NULL, "create private FFmpeg log");
	keep_log = 1;
	spawn(args, 1, 0, log);
	check(SDL_CloseIO(log), "close parent log handle");
	input = SDL_GetProcessInput(child);
	check(input != NULL, "get FFmpeg input");
	fprintf(stderr,
		"bullet: %dx%d, %s fps, %zu visible messages, "
		"%zu overlap fallbacks\n",
		width, height, fps, overlay.visible, fallbacks);
	first = last = 0;
	blank = 1;
	for (n = overlay.first; n < overlay.end; n++) {
		now = opt.start + frame_time(n, opt.fps_num, opt.fps_den);
		while (last < nmessages && messages[last].time <= now)
			last++;
		while (first < last &&
		       messages[first].time + opt.travel <= now) {
			sprite_free(messages[first].sprite);
			messages[first++].sprite = NULL;
		}
#ifdef BULLET_TEST
		if (dense_reference)
			blank = 0;
#endif
		/* Reuse transparent pixels across internal gaps. The pipe is
		 * CFR, so gaps inside the transmitted interval still need
		 * frames. */
		if (first != last || !blank)
			drawframe(now, first, last, overlay.y);
		blank = first == last;
		if (canvas->pitch == width * 4) {
			writeall(input, canvas->pixels,
				 (size_t)canvas->pitch * canvas->h);
		} else {
			for (row = 0; row < canvas->h; row++)
				writeall(input,
					 (unsigned char *)canvas->pixels +
					     row * canvas->pitch,
					 (size_t)width * 4);
		}
		ticks = SDL_GetTicks();
		if (ticks - reported >= 10000) {
			elapsed = (double)(ticks - started) / 1000;
			fprintf(
			    stderr,
			    "bullet: progress frame=%lld time=%.3f/%.3f "
			    "wall=%.1fs speed=%.2fx rgba=%.1fMiB active=%zu\n",
			    (long long)(n + 1),
			    (double)(now - opt.start) / SECOND,
			    (double)opt.duration / SECOND, elapsed,
			    (double)(now - opt.start) / SECOND / elapsed,
			    (double)surface_bytes / (1024 * 1024),
			    last - first);
			reported = ticks;
		}
	}
	fprintf(stderr, "bullet: submitted %lld frames; waiting for FFmpeg\n",
		(long long)(n - overlay.first));
	check(SDL_CloseIO(input), "close FFmpeg input");
	waitchild();
	check_destination();
	if (!commitfile(stage, opt.output, opt.force))
		die("cannot commit output; existing files were not removed");
	keep_log = 0;
	endwork();
	fprintf(stderr, "bullet: rendered %s\n", opt.output);
}

static const char *
executable(const char *variable, const char *fallback)
{
	const char *s;

	s = SDL_getenv(variable);
	return s && *s ? s : fallback;
}

static void
download(void)
{
	CURLU *url;
	char *host, *scheme, *path, *id, *chat, *video, *output, *p, *last;
	char selection[256];
	const char *extensions[] = {"mp4", "mkv", "webm"};
	const char *args[32];
	size_t i, n, length;
	int twitch;

	url = curl_url();
	if (!url || curl_url_set(url, CURLUPART_URL, opt.url, 0) ||
	    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
	    curl_url_get(url, CURLUPART_HOST, &host, 0) ||
	    curl_url_get(url, CURLUPART_PATH, &path, 0))
		die("expected a public HTTPS archive URL");
	if (strcmp(scheme, "https"))
		die("expected a public HTTPS archive URL");
	twitch = !strcmp(host, "twitch.tv") ||
		 !strcmp(host, "www.twitch.tv") ||
		 !strcmp(host, "m.twitch.tv");
	if (!twitch && strcmp(host, "youtube.com") &&
	    strcmp(host, "www.youtube.com") && strcmp(host, "m.youtube.com") &&
	    strcmp(host, "music.youtube.com") && strcmp(host, "youtu.be"))
		die("only YouTube and Twitch archive URLs are supported");
	id = NULL;
	if (twitch) {
		if (strncmp(path, "/videos/", 8))
			die("Twitch requires /videos/<id>");
		id = copystr(path + 8);
		length = strlen(id);
		if (length && id[length - 1] == '/')
			id[--length] = 0;
		if (!length)
			die("missing Twitch VOD id");
		for (i = 0; i < length; i++)
			if (!isdigit((unsigned char)id[i]))
				die("invalid Twitch VOD id");
	}
	curl_free(scheme);
	curl_free(host);
	curl_free(path);
	curl_url_cleanup(url);
	check(SDL_CreateDirectory(opt.dir), "create download directory");
	if (twitch) {
		chat = format("%s/v%s.chat.json", opt.dir, id);
		if (!exists(chat)) {
			beginwork(chat);
			stage = format("%s/chat.json", workdir);
			args[0] = executable("TWITCH_DOWNLOADER_CLI",
					     "TwitchDownloaderCLI");
			args[1] = "chatdownload";
			args[2] = "--id";
			args[3] = id;
			args[4] = "--output";
			args[5] = stage;
			args[6] = "--embed-images";
			args[7] = "--collision";
			args[8] = "Exit";
			args[9] = NULL;
			spawn(args, 0, 0, NULL);
			waitchild();
			readchat(stage);
			freechat();
			if (!commitfile(stage, chat, 0))
				die("cannot commit chat without overwriting "
				    "%s",
				    chat);
			endwork();
		} else {
			readchat(chat);
			freechat();
		}
		free(chat);
		for (i = 0; i < sizeof extensions / sizeof *extensions; i++) {
			video =
			    format("%s/v%s.%s", opt.dir, id, extensions[i]);
			if (exists(video)) {
				probe(video);
				fprintf(stderr, "bullet: reusing %s\n", video);
				free(video);
				free(id);
				return;
			}
			free(video);
		}
	}
	snprintf(selection, sizeof selection,
		 "bv[height<=%d][ext=mp4]+ba[ext=m4a]/bv[height<=%d]+ba/"
		 "b[height<=%d]",
		 opt.max_height, opt.max_height, opt.max_height);
	n = 0;
	args[n++] = executable("YT_DLP", "yt-dlp");
	args[n++] = "--no-playlist";
	args[n++] = "--no-overwrites";
	if (!twitch) {
		args[n++] = "--write-subs";
		args[n++] = "--sub-langs";
		args[n++] = "live_chat";
		args[n++] = "--sub-format";
		args[n++] = "json";
	}
	args[n++] = "--print";
	args[n++] = "after_move:filepath";
	args[n++] = "-f";
	args[n++] = selection;
	args[n++] = "-P";
	args[n++] = opt.dir;
	args[n++] = "-o";
	args[n++] = "%(id)s.%(ext)s";
	args[n++] = opt.url;
	args[n] = NULL;
	output = capture(args);
	last = NULL;
	for (p = output; *p;) {
		video = p;
		while (*p && *p != '\r' && *p != '\n')
			p++;
		while (*p == '\r' || *p == '\n')
			*p++ = 0;
		if (*video)
			last = video;
	}
	if (!last)
		die("yt-dlp returned no output path");
	video = exists(last) ? copystr(last) : format("%s/%s", opt.dir, last);
	probe(video);
	if (twitch) {
		chat = format("%s/v%s.chat.json", opt.dir, id);
	} else {
		p = copystr(basenameof(video));
		last = strrchr(p, '.');
		if (last)
			*last = 0;
		chat = format("%s/%s.live_chat.json", opt.dir, p);
		free(p);
	}
	readchat(chat);
	freechat();
	fprintf(stderr, "bullet: downloaded %s\n", video);
	free(chat);
	free(video);
	free(output);
	free(id);
}

static double
argument_number(const char *s)
{
	char *end;
	double n;

	errno = 0;
	n = strtod(s, &end);
	if (errno || end == s || *end || !isfinite(n))
		die("invalid number: %s", s);
	return n;
}

static int
argument_int(const char *s, int min, int max)
{
	double n;

	n = argument_number(s);
	if (n < min || n > max || floor(n) != n)
		die("expected an integer in %d..%d: %s", min, max, s);
	return (int)n;
}

static void
usage(void)
{
	puts("usage: bullet download URL [--dir DIR] [--max-height N]\n"
	     "       bullet render VIDEO [CHAT] [--output FILE] [options]\n"
	     "       bullet --version\n"
	     "render options:\n"
	     "  --start SECONDS       --duration SECONDS\n"
	     "  --font FILE           --fps NUM[/DEN]\n"
	     "  --output-height N     --travel-time SECONDS\n"
	     "  --opacity 0..100      --text-style outline|shadow\n"
	     "  --hls-start RFC3339   --force\n"
	     "  -h, --help");
}

static void
render_paths(void)
{
	const char *name, *dot;
	char *stem, *twitch, *youtube;
	int has_twitch, has_youtube;

	name = basenameof(opt.video);
	dot = strrchr(name, '.');
	if (!dot || dot == name)
		die("cannot infer paths: VIDEO has no extension");
	stem = copystr(opt.video);
	stem[dot - opt.video] = 0;
	if (!opt.chat) {
		twitch = format("%s.chat.json", stem);
		youtube = format("%s.live_chat.json", stem);
		has_twitch = exists(twitch);
		has_youtube = exists(youtube);
		if (has_twitch == has_youtube)
			die(has_twitch
				? "both chat formats exist; specify CHAT"
				: "no chat beside VIDEO; specify CHAT");
		inferred_chat = has_twitch ? twitch : youtube;
		free(has_twitch ? youtube : twitch);
		opt.chat = inferred_chat;
	}
	if (!opt.output) {
		inferred_output = format("%s.bullet.mp4", stem);
		opt.output = inferred_output;
	}
	free(stem);
}

static int
arguments(int argc, char **argv)
{
	const char *key, *value;
	int i, mode;

	memset(&opt, 0, sizeof opt);
	opt.duration = -1;
	opt.travel = default_travel;
	opt.opacity = default_opacity;
	opt.dir = "data";
	opt.max_height = 720;
	if (argc < 2)
		die("expected download or render; use --help");
	mode = !strcmp(argv[1], "download") ? 1
	       : !strcmp(argv[1], "render") ? 2
					    : 0;
	if (!mode)
		die("unknown command: %s", argv[1]);
	for (i = 2; i < argc; i++) {
		key = argv[i];
		if (!strcmp(key, "--force") && mode == 2) {
			opt.force = 1;
			continue;
		}
		if (*key != '-') {
			if (mode == 1 && !opt.url)
				opt.url = key;
			else if (mode == 2 && !opt.video)
				opt.video = key;
			else if (mode == 2 && !opt.chat)
				opt.chat = key;
			else
				die("unexpected argument: %s", key);
			continue;
		}
		if (i + 1 == argc)
			die("missing value for %s", key);
		value = argv[++i];
		if (mode == 1 && !strcmp(key, "--dir"))
			opt.dir = value;
		else if (mode == 1 && !strcmp(key, "--max-height"))
			opt.max_height = argument_int(value, 1, 16000);
		else if (mode == 2 && !strcmp(key, "--output"))
			opt.output = value;
		else if (mode == 2 && !strcmp(key, "--font"))
			opt.font = value;
		else if (mode == 2 && !strcmp(key, "--start"))
			opt.start = microseconds(argument_number(value));
		else if (mode == 2 && !strcmp(key, "--duration"))
			opt.duration = microseconds(argument_number(value));
		else if (mode == 2 && !strcmp(key, "--travel-time"))
			opt.travel = microseconds(argument_number(value));
		else if (mode == 2 && !strcmp(key, "--opacity"))
			opt.opacity = argument_int(value, 0, 100);
		else if (mode == 2 && !strcmp(key, "--output-height"))
			opt.height = argument_int(value, 2, 16000);
		else if (mode == 2 && !strcmp(key, "--fps"))
			rate(value, &opt.fps_num, &opt.fps_den);
		else if (mode == 2 && !strcmp(key, "--hls-start")) {
			opt.hls = 1;
			opt.origin = rfc3339(value);
		} else if (mode == 2 && !strcmp(key, "--text-style")) {
			if (strcmp(value, "outline") &&
			    strcmp(value, "shadow"))
				die("--text-style must be outline or shadow");
			opt.shadow = !strcmp(value, "shadow");
		} else {
			die("unknown option: %s", key);
		}
	}
	if ((mode == 1 && !opt.url) || (mode == 2 && !opt.video))
		die("missing arguments; use --help");
	if (mode == 2 && (!opt.chat || !opt.output))
		render_paths();
	if (!opt.duration || opt.travel < 1000)
		die("duration must be positive; travel-time must be >= 0.001 "
		    "s");
	return mode;
}

#ifndef BULLET_TEST
int
main(int argc, char **argv)
{
	int i, mode;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage();
			return 0;
		}
	}
	if (argc == 2 && !strcmp(argv[1], "--version")) {
		puts("bullet 0.1.0");
		return 0;
	}
	atexit(cleanup);
	check(SDL_Init(0), "initialize SDL");
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		die("initialize HTTP library");
	mode = arguments(argc, argv);
	if (mode == 1)
		download();
	else
		render();
	return 0;
}
#endif
