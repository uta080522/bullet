/* White-box tests, an exact-pixel encoder, and offline download tools. */
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
	if (!strcmp(argv[1], "chatdownload")) {
		copyfile(SDL_getenv("BULLET_FIXTURE_CHAT"),
			 valueof(argc, argv, "--output"));
	} else {
		dir = valueof(argc, argv, "-P");
		check(SDL_CreateDirectory(dir),
		      "create fake download directory");
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
	/* Leave written output behind for the real owner to handle. */
	if (SDL_getenv("BULLET_TOOL_FAIL") && *SDL_getenv("BULLET_TOOL_FAIL"))
		die("test downloader failed");
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

/* Original per-pixel implementation, independent of the optimized blitters. */
static void
reference_paste(SDL_Surface *to, const SDL_Surface *from, int x, int y)
{
	int sx, sy, left, top, right, bottom, c;
	unsigned char *d;
	const unsigned char *s;
	unsigned int sa, da, inv, out, value;

	left = x < 0 ? -x : 0;
	top = y < 0 ? -y : 0;
	right = from->w < to->w - x ? from->w : to->w - x;
	bottom = from->h < to->h - y ? from->h : to->h - y;
	for (sy = top; sy < bottom; sy++) {
		s = (const unsigned char *)from->pixels + sy * from->pitch;
		d = (unsigned char *)to->pixels + (y + sy) * to->pitch;
		for (sx = left; sx < right; sx++) {
			sa = s[sx * 4 + 3];
			if (!sa)
				continue;
			da = d[(x + sx) * 4 + 3];
			inv = 255 - sa;
			out = sa + (da * inv + 127) / 255;
			for (c = 0; c < 3; c++) {
				value = s[sx * 4 + c] * sa * 255 +
					d[(x + sx) * 4 + c] * da * inv;
				value = (value + out * 255 / 2) / (out * 255);
				d[(x + sx) * 4 + c] =
				    (unsigned char)(value > 255 ? 255 : value);
			}
			d[(x + sx) * 4 + 3] = (unsigned char)out;
		}
	}
}

/* Floating-point destination sampling, independent of the fixed-point
 * span interpolation and sparse run representation. */
static void
reference_subpixel_paste(SDL_Surface *to, const SDL_Surface *from, double x,
			 int y)
{
	const unsigned char clear[4] = {0};
	const unsigned char *a, *b, *row;
	unsigned char *p;
	SDL_Surface *sample;
	double position, fraction, alpha, value;
	int dx, dy, sx, sy, c;

	sample = surface(to->w, to->h);
	for (dy = 0; dy < to->h; dy++) {
		sy = dy - y;
		if (sy < 0 || sy >= from->h)
			continue;
		row = (const unsigned char *)from->pixels + sy * from->pitch;
		for (dx = 0; dx < to->w; dx++) {
			position = dx - x;
			sx = (int)floor(position);
			fraction = position - sx;
			a = sx >= 0 && sx < from->w ? row + sx * 4 : clear;
			b = sx + 1 >= 0 && sx + 1 < from->w
				? row + (sx + 1) * 4
				: clear;
			alpha = a[3] * (1 - fraction) + b[3] * fraction;
			if (!alpha)
				continue;
			p = (unsigned char *)sample->pixels +
			    dy * sample->pitch + dx * 4;
			for (c = 0; c < 3; c++) {
				value = a[c] * a[3] * (1 - fraction) +
					b[c] * b[3] * fraction;
				p[c] = (unsigned char)round(value / alpha);
			}
			p[3] = (unsigned char)round(alpha);
		}
	}
	reference_paste(to, sample, 0, 0);
	surface_free(sample);
}

static void
surfaces_equal(const SDL_Surface *a, const SDL_Surface *b)
{
	int y;

	expect(a->w == b->w && a->h == b->h, "surface dimensions equal");
	for (y = 0; y < a->h; y++)
		expect(!memcmp((const unsigned char *)a->pixels + y * a->pitch,
			       (const unsigned char *)b->pixels + y * b->pitch,
			       (size_t)a->w * 4),
		       "RGBA pixels equal reference");
}

static void
blend_tests(void)
{
	SDL_Surface *source, *actual, *expected;
	unsigned char *s, *d;
	int x, y;
	size_t before;

	before = surface_bytes;
	source = surface(256, 256);
	actual = surface(256, 256);
	expected = surface(256, 256);
	/* All 65,536 source/destination alpha pairs, including hidden RGB. */
	for (y = 0; y < 256; y++) {
		for (x = 0; x < 256; x++) {
			s = (unsigned char *)source->pixels +
			    y * source->pitch + x * 4;
			d = (unsigned char *)actual->pixels +
			    y * actual->pitch + x * 4;
			s[0] = (unsigned char)y;
			s[1] = (unsigned char)(255 - y);
			s[2] = (unsigned char)((3 * x + 7 * y) % 256);
			s[3] = (unsigned char)x;
			d[0] = (unsigned char)x;
			d[1] = (unsigned char)(255 - x);
			d[2] = (unsigned char)((x + y) % 256);
			d[3] = (unsigned char)y;
		}
		memcpy((unsigned char *)expected->pixels + y * expected->pitch,
		       (unsigned char *)actual->pixels + y * actual->pitch,
		       256 * 4);
	}
	reference_paste(expected, source, 0, 0);
	paste(actual, source, 0, 0);
	surfaces_equal(actual, expected);
	surface_free(source);
	surface_free(actual);
	surface_free(expected);
	expect(surface_bytes == before, "blend fixtures released");
}

static void
sprite_tests(void)
{
	const double xs[] = {-80,   -64,  -63.75, -63,	 -5.75, -5,
			     -0.75, -0.5, -0.25,  0,	 0.25,	0.5,
			     0.75,  9,	  9.5,	  31.75, 32};
	const int ys[] = {-8, -5, -1, 0, 3, 8};
	SDL_Surface *source, *reference, *actual, *expected;
	Sprite *sprite;
	unsigned char *p;
	int variant, x, y, visible;
	size_t i, j, before, allocated;

	before = surface_bytes;
	for (variant = 0; variant < 5; variant++) {
		source = surface(64, 6);
		reference = surface(64, 6);
		for (y = 0; y < source->h; y++) {
			for (x = 0; x < source->w; x++) {
				p = (unsigned char *)source->pixels +
				    y * source->pitch + x * 4;
				p[0] = (unsigned char)(x * 3);
				p[1] = (unsigned char)(y * 37);
				p[2] = 255;
				visible =
				    y % 2 == 0 &&
				    (x < 8 || (x >= 29 && x < 39) || x >= 62);
				p[3] = variant == 1   ? 0
				       : variant == 2 ? 255
				       : variant == 3
					   ? (unsigned char)((x % 2) * 128)
				       : visible
					   ? (unsigned char)(x % 3 ? 128 : 255)
					   : 0;
			}
			memcpy((unsigned char *)reference->pixels +
				   y * reference->pitch,
			       (unsigned char *)source->pixels +
				   y * source->pitch,
			       64 * 4);
		}
		allocated = surface_bytes;
		if (variant == 4)
			surface_bytes = MAX_MEMORY;
		sprite = sprite_create(source);
		if (variant == 4)
			surface_bytes = allocated;
		if (variant == 0)
			expect(!sprite->pixels && sprite->bytes &&
				   surface_bytes < allocated,
			       "transparent pixels omitted and dense storage "
			       "released");
		else if (variant == 1)
			expect(!sprite->pixels && !sprite->bytes,
			       "empty sprite has no runs");
		else
			expect(sprite->pixels == source && !sprite->bytes,
			       "dense/checkerboard/budget fallback preserves "
			       "source");
		actual = surface(32, 8);
		expected = surface(32, 8);
		for (i = 0; i < sizeof xs / sizeof *xs; i++) {
			for (j = 0; j < sizeof ys / sizeof *ys; j++) {
				for (y = 0; y < actual->h; y++) {
					for (x = 0; x < actual->w; x++) {
						p = (unsigned char *)
							actual->pixels +
						    y * actual->pitch + x * 4;
						p[0] = 173;
						p[1] = (unsigned char)(x * 7);
						p[2] = (unsigned char)(y * 31);
						p[3] =
						    (unsigned char)((x * 19 +
								     y * 23) %
								    256);
					}
					memcpy(
					    (unsigned char *)expected->pixels +
						y * expected->pitch,
					    (unsigned char *)actual->pixels +
						y * actual->pitch,
					    32 * 4);
				}
				reference_subpixel_paste(expected, reference,
							 xs[i], ys[j]);
				reference_subpixel_paste(expected, reference,
							 xs[i] + 1, ys[j] + 1);
				paste_sprite(actual, sprite, xs[i], ys[j]);
				paste_sprite(actual, sprite, xs[i] + 1,
					     ys[j] + 1);
				surfaces_equal(actual, expected);
			}
		}
		surface_free(actual);
		surface_free(expected);
		surface_free(reference);
		sprite_free(sprite);
		expect(surface_bytes == before,
		       "packed and dense sprite ownership");
	}
}

static void
subpixel_motion_tests(void)
{
	const int64_t times[] = {600000, 550000, 500000, 450000};
	const unsigned char left[] = {255, 191, 128, 64};
	const unsigned char right[] = {0, 64, 128, 191};
	const unsigned char gif_expected[2][20] = {
	    {0,	  0,  0,   0, 255, 255, 255, 64, 255, 255,
	     255, 64, 255, 0, 0,   64,	255, 0,	 0,   64},
	    {255, 255, 255, 64, 255, 255, 255, 64, 0, 0,
	     255, 64,  0,   0,	255, 64,  0,   0,  0, 0}};
	SDL_Surface *image;
	Message *m;
	Asset *a;
	unsigned char *pixels;
	size_t i, index, before;

	before = surface_bytes;
	canvas = surface(4, 1);
	image = surface(1, 1);
	memset(image->pixels, 255, 4);
	m = message(0);
	m->width = 1;
	m->sprite = sprite_create(image);
	opt.travel = SECOND;
	opt.opacity = 100;
	for (i = 0; i < sizeof times / sizeof *times; i++) {
		drawframe(times[i], 0, 1, 0);
		pixels = canvas->pixels;
		expect(pixels[7] == left[i] && pixels[11] == right[i],
		       "scrolling text retains fractional pixel coverage");
		expect(pixels[3] == 0 && pixels[15] == 0,
		       "subpixel movement only covers neighboring pixels");
		expect(pixels[4] == 255 && pixels[5] == 255 &&
			   pixels[6] == 255,
		       "subpixel text preserves its straight RGB color");
	}
	drawframe(50000, 0, 1, 0);
	pixels = canvas->pixels;
	expect(pixels[15] == 64 && pixels[3] == 0 && pixels[7] == 0 &&
		   pixels[11] == 0,
	       "subpixel text enters at the right edge");
	drawframe(850000, 0, 1, 0);
	expect(pixels[3] == 191 && pixels[7] == 0 && pixels[11] == 0 &&
		   pixels[15] == 0,
	       "negative subpixel position clips at the left edge");
	opt.opacity = 50;
	drawframe(500000, 0, 1, 0);
	expect(pixels[7] == 64 && pixels[11] == 64,
	       "global opacity is applied after subpixel interpolation");

	/* Text and animated images must use the same fractional phase. */
	surface_free(canvas);
	canvas = surface(5, 1);
	lane_height = 1;
	index = asset("https://example.com/subpixel.gif", 1);
	a = &assets[index];
	a->frames = resize(NULL, 2, sizeof *a->frames);
	a->ends = resize(NULL, 2, sizeof *a->ends);
	a->count = 2;
	a->ends[0] = 450000;
	a->ends[1] = 850000;
	for (i = 0; i < 2; i++) {
		a->frames[i] = surface(1, 1);
		pixels = a->frames[i]->pixels;
		pixels[i * 2] = 255;
		pixels[3] = 255;
	}
	part(m, "", index);
	m->parts[0].x = 2;
	m->width = 3;
	for (i = 0; i < 2; i++) {
		drawframe(437500 + (int64_t)i * 125000, 0, 1, 0);
		expect(
		    !memcmp(canvas->pixels, gif_expected[i],
			    sizeof gif_expected[i]),
		    "text and GIF share subpixel movement, time and opacity");
	}
	freechat();
	surface_free(canvas);
	canvas = NULL;
	memset(&opt, 0, sizeof opt);
	expect(surface_bytes == before, "subpixel motion fixtures released");
}

static void
frame_bounds_tests(void)
{
	const int rates[][2] = {
	    {25, 1}, {30000, 1001}, {113394000, 3780913}, {1, 10}, {240, 1}};
	size_t i;
	int64_t n, time, ceiling;
	Overlay o;
	Message *m;

	for (i = 0; i < sizeof rates / sizeof *rates; i++) {
		for (n = 0; n < 10000; n++) {
			time = frame_time(n, rates[i][0], rates[i][1]);
			expect(frame_ceiling(time, rates[i][0], rates[i][1]) ==
				   n,
			       "inverse clock at a sampled microsecond");
			expect(frame_ceiling(time + 1, rates[i][0],
					     rates[i][1]) == n + 1,
			       "inverse clock just after a frame");
		}
		ceiling = frame_ceiling(MAX_TIME, rates[i][0], rates[i][1]);
		expect(frame_time(ceiling, rates[i][0], rates[i][1]) >=
			       MAX_TIME &&
			   frame_time(ceiling - 1, rates[i][0], rates[i][1]) <
			       MAX_TIME,
		       "inverse clock at the duration limit");
	}
	opt.start = 2 * SECOND;
	opt.duration = 2 * SECOND;
	opt.travel = SECOND / 4;
	opt.fps_num = 30000;
	opt.fps_den = 1001;
	lane_height = 26;
	m = message(1900000);
	m->y = 52;
	m = message(2800000);
	m->y = 26;
	o = overlay_plan(80);
	expect(o.y == 26 && o.height == 52 && o.first == 0 && o.end == 32 &&
		   o.visible == 2,
	       "overlay band includes pre-seek messages");
	opt.start = 2500000;
	o = overlay_plan(50);
	expect(o.y == 26 && o.height == 24 && o.first == 9 && o.end == 17,
	       "overlay clips the bottom lane and trims both ends");
	opt.start = 3 * SECOND;
	opt.duration = 1;
	o = overlay_plan(80);
	expect(o.first == 0 && o.end == 1, "sub-frame clip");
	opt.start = 3100000;
	o = overlay_plan(80);
	expect(o.y == 0 && o.height == 1 && o.first == 0 && o.end == 1 &&
		   o.visible == 0,
	       "empty clip retains one transparent sample");
	freechat();
	memset(&opt, 0, sizeof opt);
}

static void
width_geometry_tests(void)
{
	SDL_Surface *source;
	Message *m;
	size_t index;
	int decoded_width, decoded_height;

	emote_height = 4;
	lane_height = 6;
	gap = 2;
	font_size = 4;
	opt.travel = SECOND;
	index = asset("https://example.com/wide.png", 2);
	m = message(123456);
	part(m, "", index);
	part(m, "", index);
	expect(layout(20, 6) == 0, "image-only width scene has a free lane");
	expect(m->parts[0].x == 0 && m->parts[0].width == 8 &&
		   m->parts[1].x == 10 && m->parts[1].width == 8 &&
		   m->width == 18 && m->time == 123456,
	       "aspect two measures literal positions and preserves time");
	source = surface(2, 2);
	asset_frame(&assets[index], source, 100);
	decoded_width = assets[index].frames[0]->w;
	decoded_height = assets[index].frames[0]->h;
	surface_free(source);
	freechat();
	expect(decoded_width == 8 && decoded_height == 4,
	       "square RGBA source normalizes to metadata width eight");

	index = asset("https://example.com/tiny.png", 0.01);
	m = message(654321);
	part(m, "", index);
	part(m, "", index);
	layout(20, 6);
	expect(m->parts[0].x == 0 && m->parts[0].width == 1 &&
		   m->parts[1].x == 3 && m->parts[1].width == 1 &&
		   m->width == 4 && m->time == 654321,
	       "tiny positive aspect measures width one and preserves time");
	source = surface(2, 2);
	asset_frame(&assets[index], source, 100);
	decoded_width = assets[index].frames[0]->w;
	decoded_height = assets[index].frames[0]->h;
	surface_free(source);
	freechat();
	expect(decoded_width == 1 && decoded_height == 4,
	       "tiny positive aspect decodes to width one");
	memset(&opt, 0, sizeof opt);
	puts("unit: canonical width geometry OK");
}

static void
asset_metadata_tests(void)
{
	cJSON *json;
	char *url;
	size_t i, index;

	json = parsejson(
	    "{\"embeddedData\":{\"firstParty\":[{\"id\":\"wide\","
	    "\"width\":4,\"height\":2,\"data\":\"\"}]},\"comments\":[{"
	    "\"content_offset_seconds\":0.25,\"message\":{\"fragments\":[{"
	    "\"emoticon\":{\"emoticon_id\":\"wide\"}}]}}]}");
	read_twitch(json);
	cJSON_Delete(json);
	expect(nassets == 1 && assets[0].aspect == 2,
	       "Twitch first-party aspect precedes fragment fallback one");
	expect(asset(assets[0].url, 3) == 0 && assets[0].aspect == 2,
	       "later valid metadata retains first-seen aspect");
	emote_height = 4;
	lane_height = 6;
	gap = 2;
	font_size = 4;
	opt.travel = SECOND;
	layout(20, 6);
	expect(messages[0].parts[0].x == 0 &&
		   messages[0].parts[0].width == 8 && messages[0].width == 8 &&
		   messages[0].time == 250000,
	       "first-seen aspect determines literal message geometry");
	freechat();
	for (i = 0; i < MAX_ASSETS; i++) {
		url = format("https://example.com/capacity/%zu", i);
		index = asset(url, 2);
		free(url);
		expect(index == i, "distinct URLs fill the asset capacity");
	}
	expect(asset("https://example.com/capacity/0", 1) == 0 &&
		   asset("https://example.com/capacity/4095", 3) == 4095 &&
		   nassets == 4096 && assets[0].aspect == 2 &&
		   assets[4095].aspect == 2,
	       "valid existing URLs remain usable at full capacity");
	freechat();
	memset(&opt, 0, sizeof opt);
}

static void
width_decode_tests(void)
{
	const double aspects[] = {2, 0.01};
	const int widths[] = {8, 1};
	SDL_Surface *source;
	SDL_IOStream *io;
	Message *m;
	Asset *a;
	char hash[65], *path;
	size_t i, j, index;

	emote_height = 4;
	lane_height = 6;
	gap = 2;
	font_size = 4;
	opt.travel = SECOND;
	for (i = 0; i < 2; i++) {
		index = asset("https://example.com/width.gif", aspects[i]);
		assets[index].embedded = copystr(gif);
		m = message(123456);
		part(m, "", index);
		layout(20, 6);
		a = &assets[index];
		load_asset(a, workdir);
		expect(a->count == 2,
		       "canonical width decodes two real GIF frames");
		for (j = 0; j < 2; j++)
			expect(a->frames[j]->w == widths[i] &&
				   a->frames[j]->h == 4,
			       "every GIF frame uses canonical metadata "
			       "dimensions");
		expect(a->ends[0] == 100000 && a->ends[1] == 300000 &&
			   m->parts[0].x == 0 &&
			   m->parts[0].width == widths[i] &&
			   m->width == widths[i] && m->time == 123456,
		       "GIF normalization preserves frame delays and message "
		       "time");
		hashurl(a->url, hash);
		path = format("%s/%s", workdir, hash);
		check(SDL_RemovePath(path), "remove owned width GIF cache");
		free(path);
		freechat();

		index = asset("https://example.com/width.png", aspects[i]);
		m = message(654321);
		part(m, "", index);
		layout(20, 6);
		hashurl(assets[index].url, hash);
		path = format("%s/%s", workdir, hash);
		source = surface(2, 2);
		memset(source->pixels, 255, (size_t)source->pitch * source->h);
		io = SDL_IOFromFile(path, "wb");
		expect(io != NULL && IMG_SavePNG_IO(source, io, true),
		       "write owned square PNG width fixture");
		surface_free(source);
		load_asset(&assets[index], workdir);
		a = &assets[index];
		expect(a->count == 1 && a->frames[0]->w == widths[i] &&
			   a->frames[0]->h == 4 && m->parts[0].x == 0 &&
			   m->parts[0].width == widths[i] &&
			   m->width == widths[i] && m->time == 654321,
		       "square PNG uses literal canonical width and preserves "
		       "time");
		check(SDL_RemovePath(path), "remove owned width PNG cache");
		free(path);
		freechat();
	}
	memset(&opt, 0, sizeof opt);
	puts("unit: canonical PNG and GIF dimensions OK");
}

static void
asset_frame_tests(void)
{
	Asset a = {0};
	SDL_Surface *source, *rgba, *scaled;
	unsigned char *p;
	int size, x, y;
	size_t before, i;
	int64_t t;

	before = surface_bytes;
	emote_height = 4;
	a.target_width = 4;
	for (size = 2; size <= 4; size += 2) {
		source = surface(size, size);
		for (y = 0; y < size; y++) {
			p = (unsigned char *)source->pixels +
			    y * source->pitch;
			for (x = 0; x < size; x++) {
				p[x * 4] = (unsigned char)(x * 63);
				p[x * 4 + 1] = (unsigned char)(y * 63);
				p[x * 4 + 2] = 255;
				p[x * 4 + 3] = (unsigned char)((x + y) * 41);
			}
		}
		/* The old conversion/scaling result, including transparent
		 * RGB. */
		rgba = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
		check(rgba != NULL, "reference emote conversion");
		scaled = SDL_ScaleSurface(rgba, 4, 4, SDL_SCALEMODE_LINEAR);
		check(scaled != NULL, "reference emote scale");
		asset_frame(&a, source, 10000);
		for (y = 0; y < 4; y++)
			expect(!memcmp((unsigned char *)scaled->pixels +
					   y * scaled->pitch,
				       (unsigned char *)a.frames[a.count - 1]
					       ->pixels +
					   y * a.frames[a.count - 1]->pitch,
				       16),
			       "adopted emote pixels equal copied pixels");
		SDL_DestroySurface(rgba);
		SDL_DestroySurface(scaled);
		surface_free(source);
	}
	for (i = 0; i < a.count; i++)
		surface_free(a.frames[i]);
	free(a.frames);
	free(a.ends);
	expect(surface_bytes == before, "adopted surfaces are accounted once");

	a.count = MAX_FRAMES;
	a.frames = resize(NULL, a.count, sizeof *a.frames);
	a.ends = resize(NULL, a.count, sizeof *a.ends);
	for (i = 0; i < a.count; i++) {
		a.frames[i] = surface(1, 1);
		a.ends[i] =
		    (i ? a.ends[i - 1] : 0) + (int64_t)(i % 7 + 1) * 1000;
	}
	for (t = 0; t < 3 * a.ends[a.count - 1]; t += 1000) {
		for (i = 0; t % a.ends[a.count - 1] >= a.ends[i]; i++)
			;
		expect(frame_at(&a, t) == a.frames[i],
		       "binary GIF lookup equals linear lookup at boundaries "
		       "and loops");
	}
	for (i = 0; i < a.count; i++)
		surface_free(a.frames[i]);
	free(a.frames);
	free(a.ends);
	expect(surface_bytes == before, "test GIF surfaces released");
}

static void
cropped_frame_test(int64_t now)
{
	SDL_Surface *full;
	int row;

	full = canvas;
	messages[0].y = lane_height;
	drawframe(now, 0, 1, 0);
	canvas = surface(full->w, lane_height);
	drawframe(now, 0, 1, lane_height);
	for (row = 0; row < canvas->h; row++)
		expect(
		    !memcmp((unsigned char *)canvas->pixels +
				row * canvas->pitch,
			    (unsigned char *)full->pixels +
				(row + lane_height) * full->pitch,
			    (size_t)canvas->w * 4),
		    "cropped RGBA band equals full text/GIF/alpha rendering");
	surface_free(canvas);
	canvas = full;
	messages[0].y = 0;
}

static void
cache_cleanup_tests(void)
{
#ifdef _WIN32
	char *directory, *payload;
	wchar_t *w;
	HANDLE held;

	expect(!cache_stage.directory && !cache_stage.payload,
	       "no active cache stage before cleanup fixture");
	cache_stage.directory = private_directory(workdir);
	cache_stage.payload = format("%s/payload", cache_stage.directory);
	directory = copystr(cache_stage.directory);
	payload = copystr(cache_stage.payload);
	writefile(payload, "x", 1);
	w = wide(payload);
	held = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	SDL_free(w);
	expect(held != INVALID_HANDLE_VALUE,
	       "hold cache payload without delete sharing");
	end_cache_stage();
	expect(CloseHandle(held) != 0, "release held cache payload");
	if (!cache_stage.directory)
		fprintf(stderr, "unit: cleanup fixture %s\n", workdir);
	expect(cache_stage.directory && cache_stage.payload && exists(payload),
	       "failed cache cleanup retains owner");
	end_cache_stage();
	expect(!cache_stage.directory && !cache_stage.payload &&
		   !exists(directory) && !exists(payload),
	       "cache cleanup retry removes owned paths");
	free(directory);
	free(payload);
#endif
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

	blend_tests();
	sprite_tests();
	subpixel_motion_tests();
	frame_bounds_tests();
	width_geometry_tests();
	asset_metadata_tests();
	asset_frame_tests();
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
	cache_cleanup_tests();
	width_decode_tests();
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
	assets[index].target_width = emote_height;
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
	drawframe(4850000, 0, 1, 0);
	r = colour_x(0);
	w = colour_x(2);
	drawframe(4950000, 0, 1, 0);
	blue = colour_x(1);
	w2 = colour_x(2);
	expect(r - blue == w - w2 && r > blue,
	       "text and animated image share position and time");
	cropped_frame_test(4950000);
	opt.shadow = 1;
	sprite_free(m->sprite);
	m->sprite = NULL;
	drawframe(4950000, 0, 1, 0);
	expect(colour_x(1) == blue, "shadow does not move emote");
	cropped_frame_test(4850000);
	freechat();
	index = asset("https://example.com/animated.gif", 1);
	assets[index].target_width = emote_height;
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
command_case(int success, const char *error, const char *program, va_list ap)
{
	const char *args[80], *arg;
	char *path;
	unsigned char *diagnostic;
	SDL_IOStream *log;
	size_t n, i, length;
	int status;

	args[0] = program;
	n = 1;
	while ((arg = va_arg(ap, const char *)) != NULL) {
		expect(n + 1 < sizeof args / sizeof *args, "argument limit");
		args[n++] = arg;
	}
	args[n] = NULL;
	path = format("%s/command.stderr", workdir);
	log = SDL_IOFromFile(path, "wb");
	check(log != NULL, "create test command log");
	spawn(args, 0, 0, log);
	check(SDL_CloseIO(log), "close test command log");
	check(SDL_WaitProcess(child, true, &status), "wait for test command");
	SDL_DestroyProcess(child);
	child = NULL;
	diagnostic = readfile(path, MAX_JSON, &length);
	free(path);
	if ((status == 0) != success ||
	    (!success && !strcmp(program, cli_program) && status != 1) ||
	    (error && !strstr((const char *)diagnostic, error))) {
		for (i = 0; i < n; i++)
			fprintf(stderr, "%s ", args[i]);
		fputc('\n', stderr);
		fputs((const char *)diagnostic, stderr);
		die("expected %s, got exit %d",
		    error     ? error
		    : success ? "success"
			      : "failure",
		    status);
	}
	free(diagnostic);
	cli_checks++;
}

static void
run_case(int success, const char *program, ...)
{
	va_list ap;

	va_start(ap, program);
	command_case(success, NULL, program, ap);
	va_end(ap);
}

/* Exit 1 alone also accepts failures from unrelated guards or tools. */
static void
reject_case(const char *error, const char *program, ...)
{
	va_list ap;

	va_start(ap, program);
	command_case(0, error, program, ap);
	va_end(ap);
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

static void
reference_encoder(int enabled)
{
	const char *key, *value;
	char *name, *path;

	key = SDL_getenv("BULLET_PATH_KEY");
	value = SDL_getenv(enabled ? "BULLET_REFERENCE_PATH"
				   : "BULLET_ORIGINAL_PATH");
	expect(key && value, "reference encoder environment");
	name = copystr(key);
	path = copystr(value);
	test_env("BULLET_REFERENCE_ENCODER", enabled ? "1" : "");
	test_env(name, path);
	free(name);
	free(path);
}

static char *
ffmpeg_path(const char *search)
{
#ifdef _WIN32
	wchar_t *w;
	char *utf8, *path;
	DWORD n;

	(void)search;
	w = resize(NULL, 32768, sizeof *w);
	n = SearchPathW(NULL, L"ffmpeg.exe", NULL, 32768, w, NULL);
	expect(n && n < 32768, "locate real FFmpeg");
	utf8 = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)w,
				((size_t)n + 1) * sizeof *w);
	expect(utf8 != NULL, "convert FFmpeg path");
	path = copystr(utf8);
	SDL_free(utf8);
	free(w);
	return path;
#else
	const char *p, *end;
	char *path;
	SDL_PathInfo info;
	size_t length;

	for (p = search;; p = end + 1) {
		end = strchr(p, ':');
		length = end ? (size_t)(end - p) : strlen(p);
		expect(length <= INT_MAX, "search path length");
		path = length ? format("%.*s/ffmpeg", (int)length, p)
			      : copystr("./ffmpeg");
		if (SDL_GetPathInfo(path, &info) &&
		    info.type == SDL_PATHTYPE_FILE && !access(path, X_OK))
			return path;
		free(path);
		if (!end)
			die("test tool: cannot find FFmpeg");
	}
#endif
}

static void
reference_ffmpeg(int argc, char **argv)
{
	const char *args[80];
	SDL_PropertiesID props;
	size_t n;
	int i;

	/* Windows searches the proxy's own directory before PATH. */
	reference_encoder(0);
	args[0] = SDL_getenv("BULLET_REAL_FFMPEG");
	expect(args[0] != NULL, "real FFmpeg path");
	n = 1;
	for (i = 1; i < argc; i++) {
		expect(n + 1 < sizeof args / sizeof *args, "argument limit");
		/* Exact pixel checks must not include lossy encoder artifacts.
		 * GIF decoding and other FFmpeg arguments pass through. */
		args[n++] = !strcmp(argv[i - 1], "-crf") ? "0" : argv[i];
	}
	args[n] = NULL;
	props = SDL_CreateProperties();
	check(props != 0, "create reference process properties");
	check(SDL_SetPointerProperty(
		  props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args),
	      "set reference args");
	/* SDL defaults stdin to NULL; forward the producer's RGBA pipe. */
	check(SDL_SetNumberProperty(props,
				    SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
				    SDL_PROCESS_STDIO_INHERITED),
	      "inherit reference input");
	child = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (!child)
		die("cannot start %s: %s", args[0], SDL_GetError());
	waitchild();
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

static void
media_equal(const char *a, const char *b)
{
	const char *args[] = {"ffmpeg",	  "-v",	       "error",	      "-i",
			      NULL,	  "-map",      "0",	      "-c:a",
			      "copy",	  "-fps_mode", "passthrough", "-f",
			      "framemd5", "pipe:1",    NULL};
	char *x, *y;

	args[4] = a;
	x = capture(args);
	args[4] = b;
	y = capture(args);
	if (strcmp(x, y))
		fprintf(stderr, "media mismatch: %s vs %s\n", a, b);
	expect(!strcmp(x, y), "decoded pixels, audio packets and timestamps "
			      "equal dense reference");
	free(x);
	free(y);
}

static void
render_equal(const char *bullet, const char *tool, const char *source,
	     const char *chat, const char *test_font, const char *start,
	     const char *duration, const char *travel, const char *opacity,
	     const char *style, const char *fps, const char *height)
{
	char *compact, *dense;

	compact = fixture("compact.mp4", NULL);
	dense = fixture("dense.mp4", NULL);
	/* Normal CLI cases retain the production encoder configuration. */
	reference_encoder(1);
	run_case(1, bullet, "render", source, chat, "--output", compact,
		 "--force", "--font", test_font, "--start", start,
		 "--duration", duration, "--travel-time", travel, "--opacity",
		 opacity, "--text-style", style, "--fps", fps,
		 "--output-height", height, NULL);
	run_case(1, tool, "--dense-render", source, chat, "--output", dense,
		 "--force", "--font", test_font, "--start", start,
		 "--duration", duration, "--travel-time", travel, "--opacity",
		 opacity, "--text-style", style, "--fps", fps,
		 "--output-height", height, NULL);
	reference_encoder(0);
	media_equal(compact, dense);
	audio_equal(compact, dense);
	free(compact);
	free(dense);
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

static SDL_EnumerationResult SDLCALL
count_entry(void *data, const char *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, ".") && strcmp(name, ".."))
		(*(size_t *)data)++;
	return SDL_ENUM_CONTINUE;
}

static char *cross_fixture;

static void
end_cross_fixture(void)
{
	if (cross_fixture) {
		check(
		    SDL_EnumerateDirectory(cross_fixture, remove_entry, NULL),
		    "clean cross-filesystem fixture");
		check(SDL_RemovePath(cross_fixture),
		      "remove cross-filesystem fixture");
		free(cross_fixture);
		cross_fixture = NULL;
	}
}

static char *
filesystem_id(const char *path)
{
#ifdef _WIN32
	wchar_t *w, *name;
	HANDLE handle;
	DWORD n;
	char *utf8, *id;
	size_t i;

	w = wide(path);
	handle = CreateFileW(
	    w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	SDL_free(w);
	if (handle == INVALID_HANDLE_VALUE)
		return NULL;
	name = resize(NULL, 32768, sizeof *name);
	n = GetFinalPathNameByHandleW(handle, name, 32768, VOLUME_NAME_GUID);
	CloseHandle(handle);
	if (!n || n >= 32768) {
		free(name);
		return NULL;
	}
	for (i = 0; i < n && name[i] != L'}'; i++)
		;
	if (i == n) {
		free(name);
		return NULL;
	}
	name[++i] = 0;
	utf8 = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)name,
				(i + 1) * sizeof *name);
	expect(utf8 != NULL, "convert volume identity");
	id = copystr(utf8);
	SDL_free(utf8);
	free(name);
	return id;
#else
	struct stat st;

	if (stat(path, &st))
		return NULL;
	return format("%llu", (unsigned long long)st.st_dev);
#endif
}

static char *
cross_directory(const char *root)
{
	char *path;
	unsigned int attempt;
	int ok;
#ifdef _WIN32
	wchar_t *w;
	DWORD error;
#else
	int error;
#endif

	for (attempt = 0; attempt < 100; attempt++) {
		path = format("%s/.bullet-cross-%llu-%u", root,
			      (unsigned long long)SDL_GetPerformanceCounter(),
			      attempt);
#ifdef _WIN32
		w = wide(path);
		ok = CreateDirectoryW(w, NULL);
		error = GetLastError();
		SDL_free(w);
#else
		ok = mkdir(path, 0700) == 0;
		error = errno;
#endif
		if (ok)
			return path;
		free(path);
#ifdef _WIN32
		if (error != ERROR_ALREADY_EXISTS)
#else
		if (error != EEXIST)
#endif
			return NULL;
	}
	return NULL;
}

static void
cross_cache_tests(const char *bullet, const char *source, const char *original,
		  const char *test_font)
{
	const char *roots[] = {SDL_GetBasePath(),
#ifndef _WIN32
			       "/dev/shm",
#endif
			       NULL};
	char *input_id, *output_id, *text, *dir, *chat, *saved_chat;
	char *output, *saved_output, *cache, *encoded, *bad_chat, *bad_encoded;
	char hash[65];
	unsigned char *bytes;
	size_t i, n, entries;
#ifdef _WIN32
	wchar_t *w;
#endif

	expect(atexit(end_cross_fixture) == 0,
	       "register cross-filesystem fixture cleanup");
	input_id = filesystem_id(workdir);
	expect(input_id != NULL, "identify fixture filesystem");
	output_id = NULL;
	for (i = 0; roots[i]; i++) {
		output_id = filesystem_id(roots[i]);
		if (output_id && strcmp(input_id, output_id)) {
			cross_fixture = cross_directory(roots[i]);
			if (cross_fixture)
				break;
		}
		free(output_id);
		output_id = NULL;
	}
	if (!cross_fixture) {
		printf(
		    "cli: cross-filesystem cache SKIP, no writable distinct "
		    "filesystem among fixture temp and executable roots");
#ifndef _WIN32
		printf(" or /dev/shm");
#endif
		printf("; fixture identity %s\n", input_id);
		free(input_id);
		return;
	}
	free(output_id);
	output_id = filesystem_id(cross_fixture);
	expect(output_id && strcmp(input_id, output_id),
	       "owned output fixture is on a distinct filesystem");
	printf("cli: cross-filesystem cache EXECUTED, input/cache %s, output "
	       "%s\n",
	       input_id, output_id);
	fflush(stdout);
	free(input_id);
	free(output_id);
	dir = fixture("cross-chat", NULL);
	check(SDL_CreateDirectory(dir),
	      "create cross-filesystem chat fixture");
	chat = format("%s/chat.json", dir);
	saved_chat = format("%s/chat-before.json", dir);
	text = format(
	    "{\"comments\":[{\"content_offset_seconds\":0,"
	    "\"message\":{\"fragments\":[{\"emoticon\":{"
	    "\"emoticon_id\":\"cross-filesystem\"}}]}}],\"embeddedData\":{"
	    "\"firstParty\":[{\"id\":\"cross-filesystem\",\"data\":\"%s\"}]}}",
	    gif);
	writefile(chat, text, strlen(text));
	free(text);
	copyfile(chat, saved_chat);
	cache = format("%s/assets", dir);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/cross-filesystem/"
		"default/dark/2.0",
		hash);
	encoded = format("%s/%s", cache, hash);
	expect(!exists(cache), "unseen cache directory before cache miss");
	output = format("%s/output.mp4", cross_fixture);
	saved_output = format("%s/output-before.mp4", cross_fixture);
	writefile(output, "existing output", 15);
	run_case(1, bullet, "render", source, chat, "--output", output,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", output, "-f",
		 "null", "-", NULL);
	bytes = readfile(encoded, MAX_ASSET, &n);
	text = (char *)unbase64(gif, &i);
	expect(n == i && !memcmp(bytes, text, n),
	       "cache publishes original encoded GIF bytes");
	free(bytes);
	free(text);
	copyfile(output, saved_output);
#ifdef _WIN32
	w = wide(encoded);
	expect(SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY),
	       "make fixture cache entry read-only");
	SDL_free(w);
#else
	expect(chmod(encoded, 0400) == 0 && chmod(cache, 0500) == 0,
	       "make fixture cache read-only");
#endif
	run_case(1, bullet, "render", source, chat, "--output", output,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	samebytes(output, saved_output);
#ifdef _WIN32
	w = wide(encoded);
	expect(SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL),
	       "restore fixture cache entry attributes");
	SDL_free(w);
#else
	expect(chmod(cache, 0700) == 0 && chmod(encoded, 0600) == 0,
	       "restore fixture cache permissions");
#endif
	bad_chat = format("%s/bad-gif.json", dir);
	text = format(
	    "{\"comments\":[{\"content_offset_seconds\":0,"
	    "\"message\":{\"fragments\":[{\"emoticon\":{"
	    "\"emoticon_id\":\"cross-broken\"}}]}}],\"embeddedData\":{"
	    "\"firstParty\":[{\"id\":\"cross-broken\",\"data\":\"%.*s\"}]}}",
	    (int)strlen(gif) - 16, gif);
	writefile(bad_chat, text, strlen(text));
	free(text);
	reject_case("external command failed", bullet, "render", source,
		    bad_chat, "--output", output, "--force", "--duration",
		    "0.3", "--font", test_font, NULL);
	samebytes(output, saved_output);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/cross-broken/"
		"default/dark/2.0",
		hash);
	bad_encoded = format("%s/%s", cache, hash);
	expect(!exists(bad_encoded),
	       "invalid GIF is never published across filesystems");
	free(bad_chat);
	free(bad_encoded);
	bytes = readfile(encoded, MAX_ASSET, &n);
	text = (char *)unbase64(gif, &i);
	expect(n == i && !memcmp(bytes, text, n),
	       "cache hit and invalid GIF preserve encoded bytes");
	free(bytes);
	free(text);
	entries = 0;
	check(SDL_EnumerateDirectory(cache, count_entry, &entries),
	      "count cross-filesystem cache entries");
	expect(entries == 1,
	       "cache publication cleans its owned staging directory");
	entries = 0;
	check(SDL_EnumerateDirectory(cross_fixture, count_entry, &entries),
	      "count cross-filesystem output entries");
	expect(entries == 2,
	       "render cleans output-owned verification scratch");
	samebytes(chat, saved_chat);
	samebytes(source, original);
	free(dir);
	free(chat);
	free(saved_chat);
	free(output);
	free(saved_output);
	free(cache);
	free(encoded);
	end_cross_fixture();
}

static void
cli_tests(const char *bullet, const char *tool)
{
	const char *tmp, *test_font;
	const char *version[] = {bullet, "--version", NULL};
	char hash[65];
	char *destination, *source, *original, *chat, *result, *saved;
	char *backup, *log, *partial, *broken, *invalid, *portrait, *alias;
	char *yt, *tools, *dir, *cached, *path, *text, *other, *image;
	char *oldpath, *fake_bin, *fake_ffmpeg, *auto_video, *auto_chat;
	char *auto_output, *second_chat, *path_key, *encoder_path,
	    *real_ffmpeg;
	unsigned char *data;
	size_t n, i, entries, before_entries;
	Video v;
	SDL_Surface *png;
	SDL_IOStream *io;
	const struct {
		const char *key, *value, *error;
	} bad_options[] = {
	    {"--start", "-1", "time is outside 0..7 days"},
	    {"--start", "nan", "invalid number"},
	    {"--duration", "0", "duration must be positive"},
	    {"--opacity", "101", "expected an integer in 0..100"},
	    {"--fps", "0/0", "invalid frame rate"},
	    {"--fps", "nan", "invalid frame rate"},
	    {"--text-style", "other",
	     "--text-style must be outline or shadow"},
	    {"--output-height", "3", "output dimensions must be even"},
	    {"--hls-start", "2026-02-30T00:00:00Z",
	     "invalid RFC3339 timestamp"},
	    {"--hls-start", "2026-01-01T00:00:00",
	     "invalid RFC3339 timestamp"}};
	const struct {
		const char *text, *error;
	} bad_json[] = {
	    {"{}", "missing replayChatItemAction"},
	    {"{\"comments\":[]}", "no text or emoji messages found"},
	    {"{\"comments\":[{\"content_offset_seconds\":-1,"
	     "\"message\":{\"body\":\"bad\"}}]}",
	     "time is outside 0..7 days"},
	    {"{\"comments\":[],\"embeddedData\":{\"firstParty\":[{"
	     "\"id\":\"duplicate\",\"width\":4,\"height\":2,\"data\":\"\"},{"
	     "\"id\":\"duplicate\",\"width\":514,\"height\":2,\"data\":\"\"}]}"
	     "}",
	     "emote count or aspect ratio exceeds limit"}};
	const char *invalid_aspects[] = {"0", "-1", "257", "nan", "inf"};
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
	const char *sparse = "{\"comments\":[{\"content_offset_seconds\":0.2,"
			     "\"message\":{\"body\":\"first\"}},{\"content_"
			     "offset_seconds\":0.21,"
			     "\"message\":{\"body\":\"second\"}},{\"content_"
			     "offset_seconds\":1.2,"
			     "\"message\":{\"body\":\"after gap\"}}]}";
	const char *mixed =
	    "{\"comments\":[{\"content_offset_seconds\":0,"
	    "\"message\":{\"fragments\":[{\"text\":\"M \"},"
	    "{\"emoticon\":{\"emoticon_id\":\"1\"}},{\"text\":\" text \"},"
	    "{\"emoticon\":{\"emoticon_id\":\"2\"}}]}}]}";
#ifdef _WIN32
	wchar_t *wa, *wb;
	char **environment;
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
	path_key = copystr("PATH");
#ifdef _WIN32
	/* SDL's environment is case-sensitive, unlike Windows' Path. */
	environment = SDL_GetEnvironmentVariables(SDL_GetEnvironment());
	expect(environment != NULL, "read test environment");
	for (i = 0; environment[i]; i++) {
		if (!SDL_strncasecmp(environment[i], "PATH=", 5)) {
			environment[i][4] = 0;
			free(path_key);
			path_key = copystr(environment[i]);
			break;
		}
	}
	SDL_free(environment);
#endif
	tmp = SDL_getenv(path_key);
	expect(tmp != NULL, "PATH for CLI tests");
	oldpath = copystr(tmp);
	fake_bin = fixture("fake-bin", NULL);
	check(SDL_CreateDirectory(fake_bin), "create test bin directory");
#ifdef _WIN32
	fake_ffmpeg = format("%s/ffmpeg.exe", fake_bin);
	encoder_path = format("%s;%s", fake_bin, oldpath);
#else
	fake_ffmpeg = format("%s/ffmpeg", fake_bin);
	encoder_path = format("%s:%s", fake_bin, oldpath);
#endif
	copyfile(tool, fake_ffmpeg);
#ifndef _WIN32
	expect(chmod(fake_ffmpeg, 0700) == 0, "executable test tool");
#endif
	test_env("BULLET_PATH_KEY", path_key);
	test_env("BULLET_ORIGINAL_PATH", oldpath);
	test_env("BULLET_REFERENCE_PATH", encoder_path);
	test_env("BULLET_REFERENCE_ENCODER", "");
	real_ffmpeg = ffmpeg_path(oldpath);
	test_env("BULLET_REAL_FFMPEG", real_ffmpeg);
	free(real_ffmpeg);
	run_case(1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		 "testsrc2=s=160x90:r=30000/1001", "-f", "lavfi", "-i",
		 "sine=frequency=1000:sample_rate=48000", "-t", "2", "-c:v",
		 "libx264", "-g", "12", "-preset", "ultrafast", "-c:a", "aac",
		 "-movflags", "+faststart", source, NULL);
	copyfile(source, original);
	cross_cache_tests(bullet, source, original, test_font);
	run_case(1, bullet, "--help", NULL);
	text = capture(version);
	expect(!strcmp(text, "bullet 0.1.0\n") ||
		   !strcmp(text, "bullet 0.1.0\r\n"),
	       "CLI reports the release version");
	free(text);
	auto_video = fixture("auto.mp4", NULL);
	auto_chat = fixture("auto.chat.json", twitch);
	auto_output = fixture("auto.bullet.mp4", NULL);
	second_chat = fixture("auto.live_chat.json", NULL);
	copyfile(source, auto_video);
	run_case(1, bullet, "render", auto_video, "--font", test_font,
		 "--duration", "0.3", NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", auto_output,
		 "-f", "null", "-", NULL);
	reject_case("output exists", bullet, "render", auto_video, "--font",
		    test_font, NULL);
	writefile(second_chat, youtube, strlen(youtube));
	reject_case("both chat formats exist", bullet, "render", auto_video,
		    "--font", test_font, "--force", NULL);
	run_case(1, bullet, "render", auto_video, auto_chat, "--duration",
		 "0.3", "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(auto_chat), "remove mock Twitch chat");
	run_case(1, bullet, "render", auto_video, "--duration", "0.3",
		 "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(second_chat), "remove mock YouTube chat");
	reject_case("no chat beside VIDEO", bullet, "render", auto_video,
		    "--force", NULL);
	samebytes(auto_video, source);
	free(auto_video);
	free(auto_chat);
	free(auto_output);
	free(second_chat);
	path = fixture("sparse.data", sparse);
	/* Prefix/suffix trimming, internal gaps, nonzero crop origin, no
	 * sampled chat, sub-frame duration, scaling and rational clocks. */
	render_equal(bullet, tool, source, path, test_font, "0", "2", "0.3",
		     "50", "outline", "30000/1001", "90");
	render_equal(bullet, tool, source, path, test_font, "0.501", "0.3",
		     "0.3", "100", "shadow", "25", "90");
	render_equal(bullet, tool, source, path, test_font, "0.8", "0.2",
		     "0.3", "50", "outline", "30000/1001", "90");
	render_equal(bullet, tool, source, path, test_font, "0", "0.1", "0.3",
		     "0", "shadow", "30000/1001", "90");
	render_equal(bullet, tool, source, path, test_font, "0", "2", "0.001",
		     "50", "outline", "25", "90");
	render_equal(bullet, tool, source, path, test_font, "0.501", "0.001",
		     "0.3", "50", "outline", "113394000/3780913", "90");
	render_equal(bullet, tool, source, path, test_font, "0.05", "1.8",
		     "0.3", "50", "shadow", "113394000/3780913", "180");
	free(path);
	reject_case("no chat beside VIDEO", bullet, "render", original,
		    "--output", result, NULL);
	run_case(1, bullet, "render", source, chat, "--output", result,
		 "--font", test_font, NULL);
	run_case(1, "ffmpeg", "-v", "error", "-xerror", "-i", result, "-f",
		 "null", "-", NULL);
	v = probe(result);
	expect(v.fps_num == 30000 && v.fps_den == 1001, "CLI rational fps");
	audio_equal(source, result);
	copyfile(result, saved);
	reject_case("output exists", bullet, "render", source, chat,
		    "--output", result, NULL);
	samebytes(result, saved);
	run_case(1, bullet, "render", source, chat, "--output", result,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	reject_case("output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", source, "--force",
		    NULL);
	reject_case("output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", chat, "--force", NULL);
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
	reject_case("output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", alias, "--force",
		    NULL);
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
	/* Decoder failure can reach the pipe write or the process wait.
	 * Require the FFmpeg phase, not one timing-dependent symptom. */
	reject_case("FFmpeg log:", bullet, "render", broken, chat, "--output",
		    result, "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	free(broken);
	for (i = 0; i < sizeof bad_options / sizeof *bad_options; i++)
		reject_case(bad_options[i].error, bullet, "render", source,
			    chat, "--output", invalid, bad_options[i].key,
			    bad_options[i].value, NULL);
	for (i = 0; i < sizeof invalid_aspects / sizeof *invalid_aspects; i++)
		reject_case("emote count or aspect ratio exceeds limit", tool,
			    "--invalid-duplicate", invalid_aspects[i], NULL);
	path = fixture("bad.json", NULL);
	for (i = 0; i < sizeof bad_json / sizeof *bad_json; i++) {
		writefile(path, bad_json[i].text, strlen(bad_json[i].text));
		reject_case(bad_json[i].error, bullet, "render", source, path,
			    "--output", invalid, "--font", test_font, NULL);
	}
	/* Include the terminator: a valid JSON prefix must not hide NUL. */
	writefile(path, twitch, strlen(twitch) + 1);
	reject_case("NUL byte in JSON input", bullet, "render", source, path,
		    "--output", invalid, "--font", test_font, NULL);
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
	render_equal(bullet, tool, source, path, test_font, "0", "2", "0.5",
		     "50", "outline", "30000/1001", "90");
	render_equal(bullet, tool, source, path, test_font, "0.1", "1.8",
		     "0.5", "100", "shadow", "25", "90");
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
	run_case(1, bullet, "render", source, path, "--output", result,
		 "--force", "--start", "1", "--duration", "0.5",
		 "--travel-time", "0.5", "--font", test_font, NULL);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/broken/default/"
		"dark/2.0",
		hash);
	other = format("%s/%s", dir, hash);
	expect(!exists(other),
	       "invisible malformed embedded GIF stays undecoded");
	free(other);
	copyfile(result, saved);
	reject_case("external command failed", bullet, "render", source, path,
		    "--output", result, "--force", "--font", test_font, NULL);
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
	reject_case("external command failed", bullet, "download",
		    "https://www.twitch.tv/videos/123", "--dir", dir, NULL);
	samebytes(tools, cached);
	before_entries = 0;
	check(SDL_EnumerateDirectory(dir, count_entry, &before_entries),
	      "count files before failed download");
	test_env("BULLET_TOOL_FAIL", "1");
	reject_case("test downloader failed", bullet, "download",
		    "https://www.twitch.tv/videos/456", "--dir", dir, NULL);
	other = format("%s/v456.chat.json", dir);
	expect(!exists(other), "failed downloader does not publish chat");
	free(other);
	other = format("%s/v456.mp4", dir);
	expect(!exists(other), "chat failure stops before video download");
	free(other);
	entries = 0;
	check(SDL_EnumerateDirectory(dir, count_entry, &entries),
	      "count files after failed download");
	expect(entries == before_entries,
	       "failed downloader cleans staging directory");
	other = format("%s/v123.chat.json", dir);
	samebytes(other, chat);
	free(other);
	other = fixture("truncated-before.mp4", "truncated");
	samebytes(path, other);
	free(other);
	test_env("BULLET_TOOL_FAIL", "");
	reject_case("expected a public HTTPS archive URL", bullet, "download",
		    "http://www.youtube.com/watch?v=x", "--dir", dir, NULL);
	reject_case("only YouTube and Twitch archive URLs are supported",
		    bullet, "download", "https://example.com/video", "--dir",
		    dir, NULL);
	free(yt);
	free(tools);
	free(dir);
	free(path);
	free(cached);

	/* With reference mode off, the same stand-in exits without reading
	 * input to exercise broken-pipe cleanup. */
	test_env(path_key, encoder_path);
	copyfile(result, saved);
	reject_case("write failed", bullet, "render", source, chat, "--output",
		    result, "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	test_env(path_key, oldpath);
	free(path_key);
	free(oldpath);
	free(encoder_path);
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
		if (SDL_getenv("BULLET_REFERENCE_ENCODER") &&
		    *SDL_getenv("BULLET_REFERENCE_ENCODER") &&
		    (!SDL_strcasecmp(basenameof(argv[0]), "ffmpeg") ||
		     !SDL_strcasecmp(basenameof(argv[0]), "ffmpeg.exe"))) {
			reference_ffmpeg(argc, argv);
			return 0;
		}
		if (argc == 3 && !strcmp(argv[1], "--invalid-duplicate")) {
			asset("https://example.com/duplicate.png", 2);
			asset("https://example.com/duplicate.png",
			      strtod(argv[2], NULL));
			freechat();
			return 0;
		}
		if (!strcmp(argv[1], "--width-geometry")) {
			width_geometry_tests();
			return 0;
		}
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
		if (!strcmp(argv[1], "--dense-render")) {
			dense_reference = 1;
			argv[1] = "render";
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
