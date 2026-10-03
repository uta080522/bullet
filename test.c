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

static const char *unit_program;

static void
unit_work(App *run)
{
	const char *tmp;
	char *destination;

	expect(!run->work.directory, "fixture work owner starts empty");
	tmp = SDL_getenv("TEMP");
	if (!tmp)
		tmp = SDL_getenv("TMPDIR");
	if (!tmp)
		tmp = "/tmp";
	destination = format("%s/bullet-unit.mp4", tmp);
	beginwork(&run->work, destination);
	free(destination);
}

static void
replacement_tests(App *run)
{
	cJSON *root;

	root = parsejson(
	    "{\"comments\":[],\"embeddedData\":{\"firstParty\":[{"
	    "\"id\":\"replacement\",\"width\":2,\"height\":1,"
	    "\"data\":\"first\"},{\"id\":\"replacement\",\"width\":3,"
	    "\"height\":1,\"data\":\"second\"}]}}");
	read_twitch(&run->chat, 0, 0, root);
	cJSON_Delete(root);
	expect(run->chat.nassets == 1 && run->chat.assets[0].aspect == 2 &&
		   !strcmp(run->chat.assets[0].embedded, "second"),
	       "repeated embedded URL replaces data but retains first aspect");
}

static void
replacement_oom(App *run)
{
	cJSON *root;

	root = parsejson("{\"comments\":[],\"embeddedData\":{\"firstParty\":[{"
			 "\"id\":\"replacement\",\"width\":2,\"height\":1,"
			 "\"data\":\"owned-before-replacement\"}]}}");
	read_twitch(&run->chat, 0, 0, root);
	cJSON_Delete(root);
	expect(run->chat.nassets == 1 && run->chat.assets[0].embedded &&
		   !strcmp(run->chat.assets[0].embedded,
			   "owned-before-replacement") &&
		   run->chat.assets[0].aspect == 2,
	       "valid owned embedded value before replacement OOM");
	root = parsejson("{\"comments\":[],\"embeddedData\":{\"firstParty\":[{"
			 "\"id\":\"replacement\",\"width\":3,\"height\":1,"
			 "\"data\":\"replacement\"}]}}");
	run->fail_embedded_replacement = 1;
	read_twitch(&run->chat, 0, 0, root);
	cJSON_Delete(root);
	die("test failed: replacement allocation did not fail");
}

static int
colour_x(App *run, int colour)
{
	Renderer *renderer = &run->renderer;
	int x, y, found, match;
	unsigned char *p;

	found = INT_MAX;
	for (y = 0; y < renderer->canvas->h; y++) {
		p = (unsigned char *)renderer->canvas->pixels +
		    y * renderer->canvas->pitch;
		for (x = 0; x < renderer->canvas->w; x++) {
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
reference_subpixel_paste(App *run, SDL_Surface *to, const SDL_Surface *from,
			 double x, int y)
{
	Renderer *renderer = &run->renderer;
	const unsigned char clear[4] = {0};
	const unsigned char *a, *b, *row;
	unsigned char *p;
	SDL_Surface *sample = NULL;
	double position, fraction, alpha, value;
	int dx, dy, sx, sy, c;

	surface_create(renderer, &sample, to->w, to->h);
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
	surface_destroy(renderer, &sample);
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
storage_tests(App *run)
{
	Renderer *renderer = &run->renderer;
	Chat *chat = &run->chat;
	SDL_Surface *temporary, *converted, *padded, *rejected;
	unsigned char rows[32] = {0};
	Message *m;
	Asset *a;
	size_t index;

	expect(renderer->retained.bytes == 0, "empty retained image storage");
	temporary = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(temporary != NULL, "create uncharged temporary fixture");
	memset(temporary->pixels, 255, 16);
	expect(image_charge(&renderer->retained, MAX_MEMORY),
	       "reserve full storage for capacity rejection");
	expect(image_dimensions(temporary->w, temporary->h),
	       "temporary dimensions are valid at full retained capacity");
	converted = SDL_ConvertSurface(temporary, SDL_PIXELFORMAT_RGBA32);
	check(converted != NULL, "convert uncharged temporary fixture");
	expect(renderer->retained.bytes == MAX_MEMORY &&
		   !memcmp(temporary->pixels, converted->pixels, 16),
	       "temporary conversion is excluded and preserves source pixels");
	rejected = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(rejected != NULL, "create rejected adoption fixture");
	rejected->refcount++;
	expect(!surface_take_new(renderer, &renderer->canvas, rejected) &&
		   !renderer->canvas && rejected->refcount == 1 &&
		   renderer->retained.bytes == MAX_MEMORY,
	       "capacity rejection destroys new reference without changing "
	       "owner");
	SDL_DestroySurface(rejected);
	expect(!image_charge(&renderer->retained, SIZE_MAX) &&
		   renderer->retained.bytes == MAX_MEMORY,
	       "oversized charge cannot wrap full storage");
	expect(image_release(&renderer->retained, MAX_MEMORY),
	       "release exact synthetic capacity reservation");
	SDL_DestroySurface(converted);

	rejected = SDL_CreateSurface(65537, 1, SDL_PIXELFORMAT_RGBA32);
	check(rejected != NULL, "create bounded oversized dimension fixture");
	expect(!image_dimensions(rejected->w, rejected->h),
	       "actual temporary wider than 65536 fails the dimension bound");
	expect(!surface_take_new(renderer, &renderer->canvas, rejected) &&
		   !renderer->canvas && renderer->retained.bytes == 0,
	       "invalid dimension adoption leaves no retained storage");

	padded = SDL_CreateSurfaceFrom(3, 2, SDL_PIXELFORMAT_RGBA32, rows, 16);
	check(padded != NULL, "create actual padded pitch fixture");
	expect(padded->pitch == 16 && padded->w == 3 && padded->h == 2,
	       "padded fixture has twelve visible bytes in each sixteen-byte "
	       "row");
	expect(image_charge(&renderer->retained, MAX_MEMORY - 24),
	       "reserve all but twenty-four bytes for padded rejection");
	padded->refcount++;
	expect(!surface_take_new(renderer, &renderer->canvas, padded) &&
		   !renderer->canvas && padded->refcount == 1 &&
		   renderer->retained.bytes == MAX_MEMORY - 24,
	       "twenty-four spare bytes cannot fit thirty-two actual padded "
	       "bytes");
	expect(image_release(&renderer->retained, MAX_MEMORY - 24),
	       "release exact padded capacity reservation");
	expect(!image_charge(&renderer->retained, SIZE_MAX) &&
		   renderer->retained.bytes == 0,
	       "oversized charge cannot wrap empty storage");
	expect(
	    surface_take_new(renderer, &renderer->canvas, padded) &&
		renderer->retained.bytes == 32,
	    "canvas adoption charges thirty-two bytes including row padding");
	expect(!surface_take_new(renderer, &renderer->canvas, padded) &&
		   renderer->canvas == padded &&
		   renderer->retained.bytes == 32,
	       "same-owner readoption cannot destroy or double-charge pixels");
	rejected = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(rejected != NULL, "create occupied-owner rejection fixture");
	rejected->refcount++;
	expect(!surface_take_new(renderer, &renderer->canvas, rejected) &&
		   rejected->refcount == 1 && renderer->canvas == padded &&
		   renderer->retained.bytes == 32,
	       "occupied owner rejects and destroys only the new reference");
	SDL_DestroySurface(rejected);
	surface_destroy(renderer, &renderer->canvas);
	expect(!renderer->canvas && renderer->retained.bytes == 0,
	       "canvas destruction releases the same thirty-two bytes");
	surface_create(renderer, &renderer->canvas, 2, 2);
	expect(renderer->canvas->pitch == 8 && renderer->retained.bytes == 16,
	       "created canvas charges its sixteen-byte actual footprint");
	surface_destroy(renderer, &renderer->canvas);
	surface_destroy(renderer, &renderer->canvas);
	expect(renderer->retained.bytes == 0,
	       "empty canvas destruction cannot subtract twice");

	index = asset(chat, "https://example.com/storage.png", 1);
	a = &chat->assets[index];
	a->target_width = 2;
	asset_frame(renderer, 2, a, temporary, 100);
	expect(a->count == 1 && a->ends[0] == 100000 &&
		   a->frames[0]->pitch == 8 &&
		   renderer->retained.bytes == 16 &&
		   !memcmp(temporary->pixels, a->frames[0]->pixels, 16),
	       "first normalized frame charges sixteen bytes without charging "
	       "source");
	asset_frame(renderer, 2, a, temporary, 200);
	expect(
	    a->count == 2 && a->ends[1] == 300000 &&
		renderer->retained.bytes == 32,
	    "second normalized frame adds sixteen bytes and keeps its delay");
	freechat(chat, renderer);
	expect(renderer->retained.bytes == 0,
	       "chat destruction releases every retained frame");
	SDL_DestroySurface(temporary);

	m = message(chat, 0);
	m->sprite = resize(NULL, 1, sizeof *m->sprite);
	memset(m->sprite, 0, sizeof *m->sprite);
	surface_create(renderer, &m->sprite->pixels, 8, 2);
	expect(renderer->retained.bytes == 64,
	       "dense sprite charges sixty-four bytes");
	((unsigned char *)m->sprite->pixels->pixels)[3] = 255;
	sprite_pack(renderer, m->sprite);
	expect(!m->sprite->pixels && m->sprite->bytes == 16 &&
		   renderer->retained.bytes == 16,
	       "one packed pixel and twelve-byte header replace sixty-four "
	       "bytes");
	sprite_pack(renderer, m->sprite);
	expect(renderer->retained.bytes == 16 && m->sprite->bytes == 16,
	       "already-packed sprite is unchanged");
	freechat(chat, renderer);
	expect(renderer->retained.bytes == 0,
	       "packed sprite destruction releases header");

	m = message(chat, 0);
	m->sprite = resize(NULL, 1, sizeof *m->sprite);
	memset(m->sprite, 0, sizeof *m->sprite);
	surface_create(renderer, &m->sprite->pixels, 8, 2);
	sprite_pack(renderer, m->sprite);
	expect(!m->sprite->pixels && !m->sprite->runs && !m->sprite->bytes &&
		   renderer->retained.bytes == 0,
	       "empty packing releases all sixty-four dense bytes");
	freechat(chat, renderer);

	m = message(chat, 0);
	m->sprite = resize(NULL, 1, sizeof *m->sprite);
	memset(m->sprite, 0, sizeof *m->sprite);
	surface_create(renderer, &m->sprite->pixels, 8, 2);
	((unsigned char *)m->sprite->pixels->pixels)[3] = 255;
	expect(image_charge(&renderer->retained, MAX_MEMORY - 15 - 64),
	       "reserve all but fifteen bytes beside dense sprite");
	sprite_pack(renderer, m->sprite);
	expect(m->sprite->pixels && !m->sprite->runs && !m->sprite->bytes &&
		   renderer->retained.bytes == MAX_MEMORY - 15,
	       "fifteen spare bytes cannot fit a sixteen-byte run and keep "
	       "dense pixels");
	expect(image_release(&renderer->retained, MAX_MEMORY - 15 - 64),
	       "release reservation without erasing dense sprite charge");
	sprite_pack(renderer, m->sprite);
	expect(!m->sprite->pixels && m->sprite->bytes == 16 &&
		   renderer->retained.bytes == 16,
	       "restored capacity permits dense-to-packed replacement");
	freechat(chat, renderer);
	expect(renderer->retained.bytes == 0,
	       "all retained owners are destroyed without resetting storage");
	puts("unit: retained storage literal bytes, padded pitch, rejection "
	     "and release OK");
}

static size_t release_mismatch_bytes;

static void
release_mismatch_cleanup(void)
{
	cleanup();
	if (app.chat.messages || app.chat.assets || app.chat.nmessages ||
	    app.chat.nassets || app.chat.nparts || app.renderer.canvas ||
	    app.inferred_chat || app.inferred_output ||
	    app.renderer.retained.bytes != release_mismatch_bytes) {
		fputs("test failed: mismatch teardown left owners or changed "
		      "ledger\n",
		      stderr);
		_Exit(90);
	}
	fprintf(
	    stderr,
	    "unit: mismatch teardown cleared all owners, retained %zu bytes\n",
	    release_mismatch_bytes);
}

static void
release_mismatch(App *run, int packed, int invalid, int on_exit)
{
	Renderer *renderer = &run->renderer;
	Chat *chat = &run->chat;
	SDL_Surface *source;
	Message *m;
	Asset *a;
	size_t i;

	for (i = 0; i < 2; i++) {
		m = message(chat, (int64_t)i);
		m->parts = resize(NULL, 1, sizeof *m->parts);
		m->parts[0].text = copystr("owned mismatch fixture");
		m->parts[0].asset = NONE;
		m->count = 1;
		chat->nparts++;
		m->sprite = resize(NULL, 1, sizeof *m->sprite);
		memset(m->sprite, 0, sizeof *m->sprite);
		surface_create(renderer, &m->sprite->pixels, 8, 2);
		((unsigned char *)m->sprite->pixels->pixels)[3] = 255;
		if (packed)
			sprite_pack(renderer, m->sprite);
		expect(packed ? !m->sprite->pixels && m->sprite->runs &&
				    m->sprite->bytes == 16
			      : m->sprite->pixels && !m->sprite->runs,
		       "mismatch fixture owns the requested sprite form");
	}
	a = &chat->assets[asset(chat, "https://example.com/mismatch.png", 1)];
	a->target_width = 2;
	a->embedded = copystr("owned encoded fixture");
	source = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(source != NULL, "create mismatch source");
	asset_frame(renderer, 2, a, source, 100);
	asset_frame(renderer, 2, a, source, 200);
	SDL_DestroySurface(source);
	surface_create(renderer, &renderer->canvas, 2, 2);
	run->inferred_chat = copystr("owned chat path fixture");
	run->inferred_output = copystr("owned output path fixture");
	expect(renderer->retained.bytes == (packed ? 80 : 176),
	       "mismatch fixture has the literal total footprint");
	release_mismatch_bytes = invalid ? MAX_MEMORY + 1 : 1;
	renderer->retained.bytes = release_mismatch_bytes;
	expect(atexit(release_mismatch_cleanup) == 0,
	       "register mismatch cleanup observer");
	if (on_exit)
		exit(23);
	check(freechat(chat, renderer), "release chat image storage");
	die("test failed: mismatched chat release reported success");
}

static SDL_Surface *frame_storage_source;

static void
frame_storage_cleanup(void)
{
	SDL_DestroySurface(frame_storage_source);
	frame_storage_source = NULL;
	cleanup();
	if (app.renderer.retained.bytes) {
		fprintf(
		    stderr,
		    "test failed: frame allocation left %zu retained bytes\n",
		    app.renderer.retained.bytes);
		_Exit(90);
	}
	fputs("unit: failed frame allocation leaves zero retained bytes\n",
	      stderr);
}

static void
frame_storage_oom(App *run, int allocation)
{
	size_t index;

	index = asset(&run->chat, "https://example.com/storage.png", 1);
	run->chat.assets[index].target_width = 2;
	frame_storage_source = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(frame_storage_source != NULL, "create uncharged frame source");
	expect(atexit(frame_storage_cleanup) == 0,
	       "register frame cleanup check");
	asset_frame(&run->renderer, 2, &run->chat.assets[index],
		    frame_storage_source, 100);
	expect(run->renderer.retained.bytes == 16,
	       "existing retained frame before allocation failure");
	run->fail_next_resize = allocation;
	asset_frame(&run->renderer, 2, &run->chat.assets[index],
		    frame_storage_source, 100);
	die("test failed: frame allocation did not fail");
}

static void
blend_tests(App *run)
{
	Renderer *renderer = &run->renderer;
	SDL_Surface *source = NULL, *actual = NULL, *expected = NULL;
	unsigned char *s, *d;
	int x, y;
	size_t before;

	before = renderer->retained.bytes;
	surface_create(renderer, &source, 256, 256);
	surface_create(renderer, &actual, 256, 256);
	surface_create(renderer, &expected, 256, 256);
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
	surface_destroy(renderer, &source);
	surface_destroy(renderer, &actual);
	surface_destroy(renderer, &expected);
	expect(renderer->retained.bytes == before, "blend fixtures released");
}

static void
sprite_tests(App *run)
{
	Renderer *renderer = &run->renderer;
	const double xs[] = {-80,   -64,  -63.75, -63,	 -5.75, -5,
			     -0.75, -0.5, -0.25,  0,	 0.25,	0.5,
			     0.75,  9,	  9.5,	  31.75, 32};
	const int ys[] = {-8, -5, -1, 0, 3, 8};
	SDL_Surface *source = NULL, *reference = NULL, *actual = NULL,
		    *expected = NULL;
	Sprite *sprite;
	unsigned char *p;
	int variant, x, y, visible;
	size_t i, j, before, allocated;

	before = renderer->retained.bytes;
	for (variant = 0; variant < 5; variant++) {
		surface_create(renderer, &source, 64, 6);
		surface_create(renderer, &reference, 64, 6);
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
		allocated = renderer->retained.bytes;
		if (variant == 4)
			expect(image_charge(&renderer->retained,
					    MAX_MEMORY - allocated),
			       "reserve remaining packed fallback capacity");
		sprite = resize(NULL, 1, sizeof *sprite);
		memset(sprite, 0, sizeof *sprite);
		sprite->pixels = source;
		sprite_pack(renderer, sprite);
		if (variant == 4) {
			expect(renderer->retained.bytes == MAX_MEMORY,
			       "failed packing preserves full ledger");
			expect(image_release(&renderer->retained,
					     MAX_MEMORY - allocated),
			       "release fallback reservation without erasing "
			       "owners");
		}
		if (variant == 0)
			expect(!sprite->pixels && sprite->bytes == 348 &&
				   renderer->retained.bytes == before + 1884,
			       "transparent pixels omitted and dense storage "
			       "released");
		else if (variant == 1)
			expect(!sprite->pixels && !sprite->bytes &&
				   renderer->retained.bytes == before + 1536,
			       "empty sprite releases all dense bytes and has "
			       "no runs");
		else
			expect(sprite->pixels == source && !sprite->bytes &&
				   renderer->retained.bytes == before + 3072,
			       "dense/checkerboard/budget fallback preserves "
			       "source");
		source = NULL;
		surface_create(renderer, &actual, 32, 8);
		surface_create(renderer, &expected, 32, 8);
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
				reference_subpixel_paste(
				    run, expected, reference, xs[i], ys[j]);
				reference_subpixel_paste(run, expected,
							 reference, xs[i] + 1,
							 ys[j] + 1);
				paste_sprite(actual, sprite, xs[i], ys[j]);
				paste_sprite(actual, sprite, xs[i] + 1,
					     ys[j] + 1);
				surfaces_equal(actual, expected);
			}
		}
		surface_destroy(renderer, &actual);
		surface_destroy(renderer, &expected);
		surface_destroy(renderer, &reference);
		expect(sprite_free(renderer, &sprite) && !sprite,
		       "sprite destruction clears its owner slot");
		expect(renderer->retained.bytes == before,
		       "packed and dense sprite ownership");
	}
}

static void
subpixel_motion_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &run->renderer.plan;
	const int64_t times[] = {600000, 550000, 500000, 450000};
	const unsigned char left[] = {255, 191, 128, 64};
	const unsigned char right[] = {0, 64, 128, 191};
	const unsigned char gif_expected[2][20] = {
	    {0,	  0,  0,   0, 255, 255, 255, 64, 255, 255,
	     255, 64, 255, 0, 0,   64,	255, 0,	 0,   64},
	    {255, 255, 255, 64, 255, 255, 255, 64, 0, 0,
	     255, 64,  0,   0,	255, 64,  0,   0,  0, 0}};
	SDL_Surface *image = NULL;
	Message *m;
	Asset *a;
	unsigned char *pixels;
	size_t i, index, before;

	before = renderer->retained.bytes;
	surface_create(renderer, &renderer->canvas, 4, 1);
	surface_create(renderer, &image, 1, 1);
	memset(image->pixels, 255, 4);
	m = message(scene, 0);
	m->width = 1;
	m->sprite = resize(NULL, 1, sizeof *m->sprite);
	memset(m->sprite, 0, sizeof *m->sprite);
	m->sprite->pixels = image;
	image = NULL;
	sprite_pack(renderer, m->sprite);
	plan->travel = SECOND;
	plan->opacity = 100;
	for (i = 0; i < sizeof times / sizeof *times; i++) {
		drawframe(scene, glyphs, renderer, times[i], 0, 1, 0);
		pixels = renderer->canvas->pixels;
		expect(pixels[7] == left[i] && pixels[11] == right[i],
		       "scrolling text retains fractional pixel coverage");
		expect(pixels[3] == 0 && pixels[15] == 0,
		       "subpixel movement only covers neighboring pixels");
		expect(pixels[4] == 255 && pixels[5] == 255 &&
			   pixels[6] == 255,
		       "subpixel text preserves its straight RGB color");
	}
	drawframe(scene, glyphs, renderer, 50000, 0, 1, 0);
	pixels = renderer->canvas->pixels;
	expect(pixels[15] == 64 && pixels[3] == 0 && pixels[7] == 0 &&
		   pixels[11] == 0,
	       "subpixel text enters at the right edge");
	drawframe(scene, glyphs, renderer, 850000, 0, 1, 0);
	expect(pixels[3] == 191 && pixels[7] == 0 && pixels[11] == 0 &&
		   pixels[15] == 0,
	       "negative subpixel position clips at the left edge");
	plan->opacity = 50;
	drawframe(scene, glyphs, renderer, 500000, 0, 1, 0);
	expect(pixels[7] == 64 && pixels[11] == 64,
	       "global opacity is applied after subpixel interpolation");

	/* Text and animated images must use the same fractional phase. */
	surface_destroy(renderer, &renderer->canvas);
	surface_create(renderer, &renderer->canvas, 5, 1);
	glyphs->lane_height = 1;
	index = asset(scene, "https://example.com/subpixel.gif", 1);
	a = &scene->assets[index];
	a->frames = resize(NULL, 2, sizeof *a->frames);
	a->ends = resize(NULL, 2, sizeof *a->ends);
	a->count = 2;
	a->ends[0] = 450000;
	a->ends[1] = 850000;
	for (i = 0; i < 2; i++) {
		a->frames[i] = NULL;
		surface_create(renderer, &a->frames[i], 1, 1);
		pixels = a->frames[i]->pixels;
		pixels[i * 2] = 255;
		pixels[3] = 255;
	}
	part(scene, m, "", index);
	m->parts[0].x = 2;
	m->width = 3;
	for (i = 0; i < 2; i++) {
		drawframe(scene, glyphs, renderer,
			  437500 + (int64_t)i * 125000, 0, 1, 0);
		expect(
		    !memcmp(renderer->canvas->pixels, gif_expected[i],
			    sizeof gif_expected[i]),
		    "text and GIF share subpixel movement, time and opacity");
	}
	freechat(scene, renderer);
	surface_destroy(renderer, &renderer->canvas);
	expect(renderer->retained.bytes == before,
	       "subpixel motion fixtures released");
}

static void
frame_bounds_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	RenderPlan *plan = &run->renderer.plan;
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
	plan->start = 2 * SECOND;
	plan->duration = 2 * SECOND;
	plan->travel = SECOND / 4;
	plan->fps_num = 30000;
	plan->fps_den = 1001;
	glyphs->lane_height = 26;
	m = message(scene, 1900000);
	m->y = 52;
	m = message(scene, 2800000);
	m->y = 26;
	plan->height = 80;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.y == 26 && o.height == 52 && o.first == 0 && o.end == 32 &&
		   o.visible == 2,
	       "overlay band includes pre-seek messages");
	plan->start = 2500000;
	plan->height = 50;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.y == 26 && o.height == 24 && o.first == 9 && o.end == 17,
	       "overlay clips the bottom lane and trims both ends");
	plan->start = 3 * SECOND;
	plan->duration = 1;
	plan->height = 80;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.first == 0 && o.end == 1, "sub-frame clip");
	plan->start = 3100000;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.y == 0 && o.height == 1 && o.first == 0 && o.end == 1 &&
		   o.visible == 0,
	       "empty clip retains one transparent sample");
}

static void
lane_tests(App *run)
{
	Message measured[6] = {{0}};
	size_t fallbacks;

	(void)run;
	measured[0].width = 20;
	measured[1].width = 60;
	measured[2].width = 10;
	measured[3].width = 200;
	measured[3].time = 500000;
	measured[4].width = 20;
	measured[4].time = 1000000;
	measured[5].width = 20;
	measured[5].time = 1500000;
	fallbacks = assign_lanes(measured, 6, 100, 20, 10, 2, 1000000);
	expect(fallbacks == 2 && measured[0].y == 0 && measured[1].y == 10 &&
		   measured[2].y == 0 && measured[3].y == 0 &&
		   measured[4].y == 10 && measured[5].y == 0,
	       "numeric lanes retain overflow and faster-message collisions");
	expect(
	    measured[0].time == 0 && measured[0].width == 20 &&
		measured[1].time == 0 && measured[1].width == 60 &&
		measured[2].time == 0 && measured[2].width == 10 &&
		measured[3].time == 500000 && measured[3].width == 200 &&
		measured[4].time == 1000000 && measured[4].width == 20 &&
		measured[5].time == 1500000 && measured[5].width == 20,
	    "numeric scheduling preserves literal crossing times and widths");
	fallbacks = assign_lanes(measured, 3, 100, 5, 10, 2, 1000000);
	expect(fallbacks == 2 && measured[0].y == 0 && measured[1].y == 0 &&
		   measured[2].y == 0,
	       "short output retains one lane without postponing messages");
	puts("unit: no-font numeric lane assignment OK");
}

static void
plan_tests(App *run)
{
	Options input = {0}, saved;
	Video video = {160, 90, 30000, 1001, 2000000};
	RenderPlan plan;

	(void)run;
	input.start = 501000;
	input.duration = -1;
	input.travel = 300000;
	input.height = 180;
	input.opacity = 50;
	input.shadow = 1;
	memcpy(&saved, &input, sizeof input);
	plan = resolve_plan(&input, &video);
	expect(!memcmp(&input, &saved, sizeof input) && input.duration == -1 &&
		   input.fps_num == 0 && input.fps_den == 0 &&
		   input.height == 180,
	       "plan resolution leaves all parsed option bytes unchanged");
	expect(plan.width == 320 && plan.height == 180 &&
		   plan.fps_num == 30000 && plan.fps_den == 1001 &&
		   plan.start == 501000 && plan.duration == 1499000 &&
		   plan.travel == 300000 && plan.opacity == 50 &&
		   plan.shadow == 1,
	       "plan resolves literal scaled geometry, default rate and "
	       "clipped clock");
	input.duration = 3000000;
	input.height = 0;
	input.fps_num = 113394000;
	input.fps_den = 3780913;
	memcpy(&saved, &input, sizeof input);
	plan = resolve_plan(&input, &video);
	expect(!memcmp(&input, &saved, sizeof input) && plan.width == 160 &&
		   plan.height == 90 && plan.duration == 1499000 &&
		   plan.fps_num == 113394000 && plan.fps_den == 3780913,
	       "explicit rational rate is unchanged while overlong duration "
	       "clips");
	input.start = 0;
	input.duration = 1;
	input.opacity = 0;
	input.shadow = 0;
	memcpy(&saved, &input, sizeof input);
	plan = resolve_plan(&input, &video);
	expect(
	    !memcmp(&input, &saved, sizeof input) && plan.start == 0 &&
		plan.duration == 1 && plan.travel == 300000 &&
		plan.opacity == 0 && plan.shadow == 0,
	    "zero seek and subframe duration retain literal style and clock");
	puts("unit: immutable input and resolved render plan OK");
}

static void
replan_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	RenderPlan *plan = &run->renderer.plan;
	Message *m;
	Overlay o;

	plan->start = 0;
	plan->duration = SECOND;
	plan->travel = SECOND;
	plan->fps_num = 25;
	plan->fps_den = 1;
	glyphs->lane_height = 6;
	asset(scene, "https://example.com/first.png", 1);
	asset(scene, "https://example.com/second.png", 1);
	m = message(scene, 0);
	part(scene, m, "", 0);
	m = message(scene, 2 * SECOND);
	part(scene, m, "", 1);
	plan->height = 12;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.visible == 1 && scene->assets[0].needed == 1 &&
		   scene->assets[1].needed == 0,
	       "first plan selects only the first image");
	plan->start = 2 * SECOND;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(o.visible == 1 && scene->assets[0].needed == 0 &&
		   scene->assets[1].needed == 1,
	       "replan clears stale image selection and selects second image");
	plan->start = 4 * SECOND;
	o = overlay_plan(scene, plan, glyphs->lane_height);
	expect(
	    o.visible == 0 && o.height == 1 && o.first == 0 && o.end == 1 &&
		scene->assets[0].needed == 0 && scene->assets[1].needed == 0,
	    "empty replan clears every image and keeps one transparent frame");
	puts("unit: independent visible-image replanning OK");
}

static void
width_geometry_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &run->renderer.plan;
	SDL_Surface *source = NULL;
	Message *m;
	size_t index;
	int decoded_width, decoded_height;

	glyphs->emote_height = 4;
	glyphs->lane_height = 6;
	glyphs->gap = 2;
	glyphs->font_size = 4;
	plan->travel = SECOND;
	index = asset(scene, "https://example.com/wide.png", 2);
	m = message(scene, 123456);
	part(scene, m, "", index);
	part(scene, m, "", index);
	measure(scene, glyphs);
	expect(assign_lanes(scene->messages, scene->nmessages, 20, 6,
			    glyphs->lane_height,
			    glyphs->gap > glyphs->font_size / 2
				? glyphs->gap
				: glyphs->font_size / 2,
			    plan->travel) == 0,
	       "image-only width scene has a free lane");
	expect(m->parts[0].x == 0 && m->parts[0].width == 8 &&
		   m->parts[1].x == 10 && m->parts[1].width == 8 &&
		   m->width == 18 && m->time == 123456,
	       "aspect two measures literal positions and preserves time");
	surface_create(renderer, &source, 2, 2);
	asset_frame(renderer, glyphs->emote_height, &scene->assets[index],
		    source, 100);
	decoded_width = scene->assets[index].frames[0]->w;
	decoded_height = scene->assets[index].frames[0]->h;
	surface_destroy(renderer, &source);
	freechat(scene, renderer);
	expect(decoded_width == 8 && decoded_height == 4,
	       "square RGBA source normalizes to metadata width eight");

	index = asset(scene, "https://example.com/tiny.png", 0.01);
	m = message(scene, 654321);
	part(scene, m, "", index);
	part(scene, m, "", index);
	measure(scene, glyphs);
	assign_lanes(
	    scene->messages, scene->nmessages, 20, 6, glyphs->lane_height,
	    glyphs->gap > glyphs->font_size / 2 ? glyphs->gap
						: glyphs->font_size / 2,
	    plan->travel);
	expect(m->parts[0].x == 0 && m->parts[0].width == 1 &&
		   m->parts[1].x == 3 && m->parts[1].width == 1 &&
		   m->width == 4 && m->time == 654321,
	       "tiny positive aspect measures width one and preserves time");
	surface_create(renderer, &source, 2, 2);
	asset_frame(renderer, glyphs->emote_height, &scene->assets[index],
		    source, 100);
	decoded_width = scene->assets[index].frames[0]->w;
	decoded_height = scene->assets[index].frames[0]->h;
	surface_destroy(renderer, &source);
	freechat(scene, renderer);
	expect(decoded_width == 1 && decoded_height == 4,
	       "tiny positive aspect decodes to width one");
	puts("unit: canonical width geometry OK");
}

static void
asset_metadata_tests(App *run)
{
	Options *options = &run->options;
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &run->renderer.plan;
	cJSON *json;
	char *url;
	size_t i, index;

	json = parsejson(
	    "{\"embeddedData\":{\"firstParty\":[{\"id\":\"wide\","
	    "\"width\":4,\"height\":2,\"data\":\"\"}]},\"comments\":[{"
	    "\"content_offset_seconds\":0.25,\"message\":{\"fragments\":[{"
	    "\"emoticon\":{\"emoticon_id\":\"wide\"}}]}}]}");
	read_twitch(scene, options->hls, options->origin, json);
	cJSON_Delete(json);
	expect(scene->nassets == 1 && scene->assets[0].aspect == 2,
	       "Twitch first-party aspect precedes fragment fallback one");
	expect(asset(scene, scene->assets[0].url, 3) == 0 &&
		   scene->assets[0].aspect == 2,
	       "later valid metadata retains first-seen aspect");
	glyphs->emote_height = 4;
	glyphs->lane_height = 6;
	glyphs->gap = 2;
	glyphs->font_size = 4;
	plan->travel = SECOND;
	measure(scene, glyphs);
	assign_lanes(
	    scene->messages, scene->nmessages, 20, 6, glyphs->lane_height,
	    glyphs->gap > glyphs->font_size / 2 ? glyphs->gap
						: glyphs->font_size / 2,
	    plan->travel);
	expect(scene->messages[0].parts[0].x == 0 &&
		   scene->messages[0].parts[0].width == 8 &&
		   scene->messages[0].width == 8 &&
		   scene->messages[0].time == 250000,
	       "first-seen aspect determines literal message geometry");
	freechat(scene, renderer);
	for (i = 0; i < MAX_ASSETS; i++) {
		url = format("https://example.com/capacity/%zu", i);
		index = asset(scene, url, 2);
		free(url);
		expect(index == i, "distinct URLs fill the asset capacity");
	}
	expect(asset(scene, "https://example.com/capacity/0", 1) == 0 &&
		   asset(scene, "https://example.com/capacity/4095", 3) ==
		       4095 &&
		   scene->nassets == 4096 && scene->assets[0].aspect == 2 &&
		   scene->assets[4095].aspect == 2,
	       "valid existing URLs remain usable at full capacity");
}

static void
width_decode_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &run->renderer.plan;
	OutputWork *work = &run->work;
	CacheStage *cache = &run->cache_stage;
	SDL_Process **child = &run->child;
	const double aspects[] = {2, 0.01};
	const int widths[] = {8, 1};
	SDL_Surface *source = NULL;
	SDL_IOStream *io;
	Message *m;
	Asset *a;
	char hash[65], *path;
	size_t i, j, index;

	unit_work(run);
	glyphs->emote_height = 4;
	glyphs->lane_height = 6;
	glyphs->gap = 2;
	glyphs->font_size = 4;
	plan->travel = SECOND;
	for (i = 0; i < 2; i++) {
		index =
		    asset(scene, "https://example.com/width.gif", aspects[i]);
		scene->assets[index].embedded = copystr(gif);
		m = message(scene, 123456);
		part(scene, m, "", index);
		measure(scene, glyphs);
		assign_lanes(scene->messages, scene->nmessages, 20, 6,
			     glyphs->lane_height,
			     glyphs->gap > glyphs->font_size / 2
				 ? glyphs->gap
				 : glyphs->font_size / 2,
			     plan->travel);
		a = &scene->assets[index];
		load_asset(renderer, glyphs->emote_height, work, cache, child,
			   a, work->directory);
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
		path = format("%s/%s", work->directory, hash);
		check(SDL_RemovePath(path), "remove owned width GIF cache");
		free(path);
		freechat(scene, renderer);

		index =
		    asset(scene, "https://example.com/width.png", aspects[i]);
		m = message(scene, 654321);
		part(scene, m, "", index);
		measure(scene, glyphs);
		assign_lanes(scene->messages, scene->nmessages, 20, 6,
			     glyphs->lane_height,
			     glyphs->gap > glyphs->font_size / 2
				 ? glyphs->gap
				 : glyphs->font_size / 2,
			     plan->travel);
		hashurl(scene->assets[index].url, hash);
		path = format("%s/%s", work->directory, hash);
		surface_create(renderer, &source, 2, 2);
		memset(source->pixels, 255, (size_t)source->pitch * source->h);
		io = SDL_IOFromFile(path, "wb");
		expect(io != NULL && IMG_SavePNG_IO(source, io, true),
		       "write owned square PNG width fixture");
		surface_destroy(renderer, &source);
		load_asset(renderer, glyphs->emote_height, work, cache, child,
			   &scene->assets[index], work->directory);
		a = &scene->assets[index];
		expect(a->count == 1 && a->frames[0]->w == widths[i] &&
			   a->frames[0]->h == 4 && m->parts[0].x == 0 &&
			   m->parts[0].width == widths[i] &&
			   m->width == widths[i] && m->time == 654321,
		       "square PNG uses literal canonical width and preserves "
		       "time");
		check(SDL_RemovePath(path), "remove owned width PNG cache");
		free(path);
		freechat(scene, renderer);
	}
	puts("unit: canonical PNG and GIF dimensions OK");
}

static void
asset_frame_tests(App *run)
{
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	Asset a = {0};
	SDL_Surface *source = NULL, *rgba, *scaled;
	unsigned char *p;
	int size, x, y;
	size_t before, i;
	int64_t t;

	before = renderer->retained.bytes;
	glyphs->emote_height = 4;
	a.target_width = 4;
	for (size = 2; size <= 4; size += 2) {
		surface_create(renderer, &source, size, size);
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
		asset_frame(renderer, glyphs->emote_height, &a, source, 10000);
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
		surface_destroy(renderer, &source);
	}
	for (i = 0; i < a.count; i++)
		surface_destroy(renderer, &a.frames[i]);
	free(a.frames);
	free(a.ends);
	expect(renderer->retained.bytes == before,
	       "adopted surfaces are accounted once");

	a.count = MAX_FRAMES;
	a.frames = resize(NULL, a.count, sizeof *a.frames);
	a.ends = resize(NULL, a.count, sizeof *a.ends);
	for (i = 0; i < a.count; i++) {
		a.frames[i] = NULL;
		surface_create(renderer, &a.frames[i], 1, 1);
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
		surface_destroy(renderer, &a.frames[i]);
	free(a.frames);
	free(a.ends);
	expect(renderer->retained.bytes == before,
	       "test GIF surfaces released");
}

static void
cropped_frame_test(App *run, int64_t now)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	SDL_Surface *full;
	int row;

	full = renderer->canvas;
	scene->messages[0].y = glyphs->lane_height;
	drawframe(scene, glyphs, renderer, now, 0, 1, 0);
	renderer->canvas = NULL;
	surface_create(renderer, &renderer->canvas, full->w,
		       glyphs->lane_height);
	drawframe(scene, glyphs, renderer, now, 0, 1, glyphs->lane_height);
	for (row = 0; row < renderer->canvas->h; row++)
		expect(
		    !memcmp((unsigned char *)renderer->canvas->pixels +
				row * renderer->canvas->pitch,
			    (unsigned char *)full->pixels +
				(row + glyphs->lane_height) * full->pitch,
			    (size_t)renderer->canvas->w * 4),
		    "cropped RGBA band equals full text/GIF/alpha rendering");
	surface_destroy(renderer, &renderer->canvas);
	renderer->canvas = full;
	scene->messages[0].y = 0;
}

static void
cache_cleanup_tests(App *run)
{
#ifdef _WIN32
	OutputWork *work = &run->work;
	CacheStage *cache = &run->cache_stage;
	char *directory, *payload;
	wchar_t *w;
	HANDLE held;

	unit_work(run);
	expect(!cache->directory && !cache->payload,
	       "no active cache stage before cleanup fixture");
	cache->directory = private_directory(work->directory);
	cache->payload = format("%s/payload", cache->directory);
	directory = copystr(cache->directory);
	payload = copystr(cache->payload);
	writefile(payload, "x", 1);
	w = wide(payload);
	held = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	SDL_free(w);
	expect(held != INVALID_HANDLE_VALUE,
	       "hold cache payload without delete sharing");
	end_cache_stage(cache);
	expect(CloseHandle(held) != 0, "release held cache payload");
	if (!cache->directory)
		fprintf(stderr, "unit: cleanup fixture %s\n", work->directory);
	expect(cache->directory && cache->payload && exists(payload),
	       "failed cache cleanup retains owner");
	end_cache_stage(cache);
	expect(!cache->directory && !cache->payload && !exists(directory) &&
		   !exists(payload),
	       "cache cleanup retry removes owned paths");
	free(directory);
	free(payload);
#else
	(void)run;
	puts("unit: held cache cleanup SKIP, requires Windows delete sharing");
#endif
}

static char *
fault_case(App *run, const char *const *args, int expected, const char *error,
	   const char *marker)
{
	char *path, *output;
	unsigned char *diagnostic;
	size_t length, from, to;
	int status, closed;

	path = format("%s/fault.stderr", run->work.directory);
	run->work.log = SDL_IOFromFile(path, "wb");
	check(run->work.log != NULL, "open fault log");
	spawn(&run->child, args, 0, 1, run->work.log);
	closed = SDL_CloseIO(run->work.log);
	run->work.log = NULL;
	check(closed, "close fault log");
	output = SDL_ReadProcess(run->child, &length, &status);
	check(output != NULL, "read fault process output");
	expect(length < MAX_JSON, "bounded fault output");
	SDL_DestroyProcess(run->child);
	run->child = NULL;
	diagnostic = readfile(path, MAX_JSON, &length);
	for (from = to = 0; from < length; from++)
		if (diagnostic[from] != '\r' || diagnostic[from + 1] != '\n')
			diagnostic[to++] = diagnostic[from];
	diagnostic[to] = 0;
	printf("unit: %s exit %d expected %d\n%s%s", args[1], status, expected,
	       output, diagnostic);
	expect(status == expected, "exact intentional fault status");
	expect(strstr((const char *)diagnostic, error) != NULL,
	       "intended fault diagnostic");
	expect(!strstr((const char *)diagnostic, "AddressSanitizer") &&
		   !strstr((const char *)diagnostic, "runtime error:") &&
		   !strstr((const char *)diagnostic, "test failed:"),
	       "intentional fault has no sanitizer or test error");
	if (marker)
		expect(strstr((const char *)diagnostic, marker) != NULL,
		       "fault cleanup owner-clearance marker");
	else
		expect(!strcmp((const char *)diagnostic, error),
		       "exact orderly OOM diagnostic");
	free(diagnostic);
	free(path);
	return output;
}

static void
storage_failure_tests(App *run)
{
	const char *args[] = {unit_program, "--frame-storage-oom", "1", NULL};
	char *output;
	int allocation;

	unit_work(run);
	for (allocation = 1; allocation <= 2; allocation++) {
		args[2] = allocation == 1 ? "1" : "2";
		output =
		    fault_case(run, args, 1, "bullet: out of memory\n",
			       "unit: failed frame allocation leaves zero "
			       "retained bytes\n");
		expect(!*output, "frame OOM has no stdout");
		SDL_free(output);
	}
	puts("unit: both frame owner-array allocation failures clean retained "
	     "storage OK");
}

static void
replacement_failure_tests(App *run)
{
	const char *args[] = {unit_program, "--replacement-oom", NULL};
	char *output;

	unit_work(run);
	output = fault_case(run, args, 1, "bullet: out of memory\n", NULL);
	expect(!*output, "replacement OOM has no stdout");
	SDL_free(output);
}

static void
release_failure_tests(App *run)
{
	const char *args[] = {
	    unit_program, "--release-mismatch", NULL, NULL, NULL, NULL};
	const char *forms[] = {"dense", "packed"};
	const char *ledgers[] = {"underflow", "invalid"};
	const char *boundaries[] = {"normal", "atexit"};
	char *output, *marker;
	size_t form, ledger, boundary;

	unit_work(run);
	for (form = 0; form < 2; form++) {
		for (ledger = 0; ledger < 2; ledger++) {
			for (boundary = 0; boundary < 2; boundary++) {
				args[2] = forms[form];
				args[3] = ledgers[ledger];
				args[4] = boundaries[boundary];
				marker = format(
				    "unit: mismatch teardown cleared "
				    "all owners, retained %zu bytes\n",
				    ledger ? MAX_MEMORY + 1 : (size_t)1);
				printf("unit: mismatch %s %s %s\n", args[2],
				       args[3], args[4]);
				output = fault_case(
				    run, args, boundary ? 23 : 1,
				    "retained image storage release mismatch",
				    marker);
				expect(!*output, "mismatch has no stdout");
				SDL_free(output);
				free(marker);
			}
		}
	}
}

static void
clock_hash_tests(App *run)
{
	char hash[65];
	int num, den;
	int64_t utc;

	(void)run;
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
}

static void
alpha_tests(App *run)
{
	Renderer *renderer = &run->renderer;
	SDL_Surface *a = NULL, *b = NULL;
	unsigned char *pixel;

	surface_create(renderer, &a, 1, 1);
	surface_create(renderer, &b, 1, 1);
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
	surface_destroy(renderer, &a);
	surface_destroy(renderer, &b);
}

static void
chat_clock_tests(App *run)
{
	Options *options = &run->options;
	Chat *scene = &run->chat;
	Renderer *renderer = &run->renderer;
	OutputWork *work = &run->work;
	cJSON *json;
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

	unit_work(run);
	work->stage = format("%s/chat.json", work->directory);
	writefile(work->stage, youtube, strlen(youtube));
	readchat(scene, options->hls, options->origin, work->stage);
	expect(scene->nmessages == 1 && scene->messages[0].time == 2 * SECOND,
	       "YouTube pre-stream exclusion");
	expect(scene->messages[0].count == 2 && scene->nassets == 1 &&
		   scene->assets[0].aspect == 2,
	       "mixed YouTube text and image");
	freechat(scene, renderer);
	json = parsejson("{\"comments\":[{\"content_offset_seconds\":5,"
			 "\"created_at\":\"2026-01-01T00:00:06.100Z\","
			 "\"message\":{\"body\":\"hello\"}}]}");
	read_twitch(scene, options->hls, options->origin, json);
	expect(scene->messages[0].time == 5 * SECOND,
	       "no Twitch interpolation");
	freechat(scene, renderer);
	options->hls = 1;
	options->origin = rfc3339("2026-01-01T00:00:00Z");
	read_twitch(scene, options->hls, options->origin, json);
	expect(scene->messages[0].time == 6100000, "explicit HLS clock");
	cJSON_Delete(json);
	freechat(scene, renderer);
}

static void
animated_render_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &renderer->plan;
	OutputWork *work = &run->work;
	CacheStage *cache = &run->cache_stage;
	SDL_Process **child = &run->child;
	Message *m;
	unsigned char *pixel;
	char hash[65], *cached;
	size_t index;
	int r, w, blue, w2;

	unit_work(run);
	plan->travel = 10 * SECOND;
	plan->opacity = 50;
	openfont(glyphs, SDL_getenv("BULLET_TEST_FONT"), 200, 80);
	index = asset(scene, "https://example.com/animated.gif", 1);
	scene->assets[index].embedded = copystr(gif);
	scene->assets[index].target_width = glyphs->emote_height;
	load_asset(renderer, glyphs->emote_height, work, cache, child,
		   &scene->assets[index], work->directory);
	expect(scene->assets[index].count == 2, "decode complete GIF");
	pixel = frame_at(&scene->assets[index], 50000)->pixels;
	expect(pixel[0] == 255 && pixel[2] == 0, "first GIF frame");
	pixel = frame_at(&scene->assets[index], 150000)->pixels;
	expect(pixel[0] == 0 && pixel[2] == 255, "second GIF frame");
	pixel = frame_at(&scene->assets[index], 350000)->pixels;
	expect(pixel[0] == 255, "GIF loops");
	m = message(scene, 0);
	part(scene, m, "M", NONE);
	part(scene, m, "", index);
	measure(scene, glyphs);
	expect(assign_lanes(scene->messages, scene->nmessages, 200, 80,
			    glyphs->lane_height,
			    glyphs->gap > glyphs->font_size / 2
				? glyphs->gap
				: glyphs->font_size / 2,
			    plan->travel) == 0,
	       "free lane");
	surface_create(renderer, &renderer->canvas, 200, 80);
	drawframe(scene, glyphs, renderer, 4850000, 0, 1, 0);
	r = colour_x(run, 0);
	w = colour_x(run, 2);
	drawframe(scene, glyphs, renderer, 4950000, 0, 1, 0);
	blue = colour_x(run, 1);
	w2 = colour_x(run, 2);
	expect(r - blue == w - w2 && r > blue,
	       "text and animated image share position and time");
	cropped_frame_test(run, 4950000);
	plan->shadow = 1;
	expect(sprite_free(renderer, &m->sprite) && !m->sprite,
	       "message sprite destruction clears its owner slot");
	drawframe(scene, glyphs, renderer, 4950000, 0, 1, 0);
	expect(colour_x(run, 1) == blue, "shadow does not move emote");
	cropped_frame_test(run, 4850000);
	freechat(scene, renderer);
	index = asset(scene, "https://example.com/animated.gif", 1);
	scene->assets[index].target_width = glyphs->emote_height;
	load_asset(renderer, glyphs->emote_height, work, cache, child,
		   &scene->assets[index], work->directory);
	expect(scene->assets[index].count == 2, "offline GIF cache reuse");
	hashurl(scene->assets[index].url, hash);
	cached = format("%s/%s", work->directory, hash);
	check(SDL_RemovePath(cached), "remove test cache");
	free(cached);
}

static void
text_lane_tests(App *run)
{
	Chat *scene = &run->chat;
	Glyphs *glyphs = &run->glyphs;
	Renderer *renderer = &run->renderer;
	RenderPlan *plan = &renderer->plan;
	Message *m;
	size_t i;

	openfont(glyphs, SDL_getenv("BULLET_TEST_FONT"), 200, 80);
	plan->travel = default_travel;
	for (i = 0; i < 3; i++) {
		m = message(scene, 0);
		part(scene, m, "hello", NONE);
	}
	measure(scene, glyphs);
	expect(assign_lanes(scene->messages, scene->nmessages, 120, 16,
			    glyphs->lane_height,
			    glyphs->gap > glyphs->font_size / 2
				? glyphs->gap
				: glyphs->font_size / 2,
			    plan->travel) == 2,
	       "overlap fallback");
	for (i = 0; i < 3; i++)
		expect(scene->messages[i].time == 0 &&
			   scene->messages[i].y == 0,
		       "crowding never postpones a comment");
}

static unsigned int cli_checks;
static const char *cli_program;

static void
command_case(App *run, int success, const char *error, const char *program,
	     va_list ap)
{
	OutputWork *work = &run->work;
	SDL_Process **child = &run->child;
	const char *args[80], *arg;
	char *path;
	unsigned char *diagnostic;
	size_t n, i, length;
	int status, closed;

	args[0] = program;
	n = 1;
	while ((arg = va_arg(ap, const char *)) != NULL) {
		expect(n + 1 < sizeof args / sizeof *args, "argument limit");
		args[n++] = arg;
	}
	args[n] = NULL;
	path = format("%s/command.stderr", work->directory);
	work->log = SDL_IOFromFile(path, "wb");
	check(work->log != NULL, "create test command log");
	spawn(child, args, 0, 0, work->log);
	closed = SDL_CloseIO(work->log);
	work->log = NULL;
	check(closed, "close test command log");
	check(SDL_WaitProcess(*child, true, &status), "wait for test command");
	SDL_DestroyProcess(*child);
	*child = NULL;
	diagnostic = readfile(path, MAX_JSON, &length);
	free(path);
	expect(!strstr((const char *)diagnostic, "AddressSanitizer") &&
		   !strstr((const char *)diagnostic, "runtime error:"),
	       "CLI command has no sanitizer error");
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
run_case(App *run, int success, const char *program, ...)
{
	va_list ap;

	va_start(ap, program);
	command_case(run, success, NULL, program, ap);
	va_end(ap);
}

/* Exit 1 alone also accepts failures from unrelated guards or tools. */
static void
reject_case(App *run, const char *error, const char *program, ...)
{
	va_list ap;

	va_start(ap, program);
	command_case(run, 0, error, program, ap);
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
reference_ffmpeg(App *run, int argc, char **argv)
{
	SDL_Process **child = &run->child;
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
	*child = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (!*child)
		die("cannot start %s: %s", args[0], SDL_GetError());
	waitchild(child);
}

static char *
fixture(App *run, const char *name, const char *contents)
{
	OutputWork *work = &run->work;
	char *path;

	path = format("%s/%s", work->directory, name);
	if (contents)
		writefile(path, contents, strlen(contents));
	return path;
}

static void
audio_equal(App *run, const char *a, const char *b)
{
	SDL_Process **child = &run->child;
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
	text = capture(child, args);
	x = parsejson(text);
	free(text);
	args[12] = b;
	text = capture(child, args);
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
media_equal(App *run, const char *a, const char *b)
{
	SDL_Process **child = &run->child;
	const char *args[] = {"ffmpeg",	  "-v",	       "error",	      "-i",
			      NULL,	  "-map",      "0",	      "-c:a",
			      "copy",	  "-fps_mode", "passthrough", "-f",
			      "framemd5", "pipe:1",    NULL};
	char *x, *y;

	args[4] = a;
	x = capture(child, args);
	args[4] = b;
	y = capture(child, args);
	if (strcmp(x, y))
		fprintf(stderr, "media mismatch: %s vs %s\n", a, b);
	expect(!strcmp(x, y), "decoded pixels, audio packets and timestamps "
			      "equal dense reference");
	free(x);
	free(y);
}

static void
render_equal(App *run, const char *bullet, const char *tool,
	     const char *source, const char *chat, const char *test_font,
	     const char *start, const char *duration, const char *travel,
	     const char *opacity, const char *style, const char *fps,
	     const char *height)
{
	char *compact, *dense;

	compact = fixture(run, "compact.mp4", NULL);
	dense = fixture(run, "dense.mp4", NULL);
	/* Normal CLI cases retain the production encoder configuration. */
	reference_encoder(1);
	run_case(run, 1, bullet, "render", source, chat, "--output", compact,
		 "--force", "--font", test_font, "--start", start,
		 "--duration", duration, "--travel-time", travel, "--opacity",
		 opacity, "--text-style", style, "--fps", fps,
		 "--output-height", height, NULL);
	run_case(run, 1, tool, "--dense-render", source, chat, "--output",
		 dense, "--force", "--font", test_font, "--start", start,
		 "--duration", duration, "--travel-time", travel, "--opacity",
		 opacity, "--text-style", style, "--fps", fps,
		 "--output-height", height, NULL);
	reference_encoder(0);
	media_equal(run, compact, dense);
	audio_equal(run, compact, dense);
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
	if (SDL_asprintf(&path, "%s/%s", dir, name) < 0) {
		SDL_SetError("out of memory building fixture path");
		return SDL_ENUM_FAILURE;
	}
	if (!SDL_GetPathInfo(path, &info) ||
	    (info.type == SDL_PATHTYPE_DIRECTORY &&
	     !SDL_EnumerateDirectory(path, remove_entry, NULL)) ||
	    !SDL_RemovePath(path)) {
		SDL_free(path);
		return SDL_ENUM_FAILURE;
	}
	SDL_free(path);
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

static int
end_cross_fixture(void)
{
	if (cross_fixture) {
		if (!SDL_EnumerateDirectory(cross_fixture, remove_entry,
					    NULL) ||
		    !SDL_RemovePath(cross_fixture))
			return 0;
		free(cross_fixture);
		cross_fixture = NULL;
	}
	return 1;
}

static void
cleanup_cross_fixture(void)
{
	if (!end_cross_fixture())
		fprintf(stderr,
			"bullet: cannot remove cross-filesystem fixture: %s: "
			"%s\n",
			cross_fixture, SDL_GetError());
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
held_cross_exit(void)
{
#ifdef _WIN32
	char *path;
	wchar_t *w;
	HANDLE held;

	expect(atexit(cleanup_cross_fixture) == 0,
	       "register held fixture cleanup");
	cross_fixture = cross_directory(SDL_GetBasePath());
	expect(cross_fixture != NULL, "create owned held fixture");
	path = format("%s/held", cross_fixture);
	writefile(path, "x", 1);
	w = wide(path);
	held = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	SDL_free(w);
	free(path);
	expect(held != INVALID_HANDLE_VALUE,
	       "hold cross fixture without delete sharing");
	printf("held cross fixture %s\n", cross_fixture);
	fflush(stdout);
	fprintf(stderr, "bullet: intentional held cross fixture exit\n");
	exit(23);
#else
	die("held cross fixture requires Windows delete sharing");
#endif
}

static void
cross_cleanup_retry(App *run)
{
	(void)run;
#ifdef _WIN32
	char *owner, *nested, *path;
	wchar_t *w;
	HANDLE held;

	expect(!cross_fixture, "no active cross fixture before retry test");
	expect(atexit(cleanup_cross_fixture) == 0,
	       "register retry fixture cleanup");
	cross_fixture = cross_directory(SDL_GetBasePath());
	expect(cross_fixture != NULL, "create owned retry fixture");
	owner = cross_fixture;
	nested = format("%s/nested", owner);
	check(SDL_CreateDirectory(nested), "create nested cleanup fixture");
	path = format("%s/held", nested);
	writefile(path, "x", 1);
	w = wide(path);
	held = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	SDL_free(w);
	expect(held != INVALID_HANDLE_VALUE,
	       "hold nested fixture without delete sharing");
	expect(!end_cross_fixture(), "held recursive cleanup returns failure");
	expect(cross_fixture == owner && exists(owner) && exists(nested) &&
		   exists(path),
	       "failed recursive cleanup retains owner and held tree");
	check(CloseHandle(held) != 0, "release held nested fixture");
	owner = copystr(cross_fixture);
	check(end_cross_fixture(), "retry recursive fixture cleanup");
	expect(!cross_fixture && !exists(owner) && !exists(nested) &&
		   !exists(path),
	       "released handle permits removal and clears owner");
	expect(end_cross_fixture(), "empty fixture cleanup is harmless");
	free(owner);
	free(nested);
	free(path);
	puts("unit: held cross cleanup retry passed");
#else
	puts("unit: held cross cleanup retry SKIP, requires Windows delete "
	     "sharing");
#endif
}

static void
held_exit_tests(App *run)
{
#ifdef _WIN32
	const char *args[] = {unit_program, "--held-cross-exit", NULL};
	const char *prefix = "held cross fixture ";
	char *output, *owner, *path, *allowed;
	unsigned char *bytes, *diagnostic;
	size_t n, entries;

	unit_work(run);
	output = fault_case(run, args, 23,
			    "bullet: cannot remove cross-filesystem fixture:",
			    "bullet: intentional held cross fixture exit\n");
	expect(!strncmp(output, prefix, strlen(prefix)),
	       "held child reports its exclusively owned fixture");
	owner = output + strlen(prefix);
	n = strlen(owner);
	expect(n && owner[n - 1] == '\n', "held fixture output ends once");
	owner[--n] = 0;
	if (n && owner[n - 1] == '\r')
		owner[--n] = 0;
	allowed = format("%s/.bullet-cross-", SDL_GetBasePath());
	expect(!strncmp(owner, allowed, strlen(allowed)) &&
		   owner[strlen(allowed)] &&
		   strspn(owner + strlen(allowed), "0123456789-") ==
		       strlen(owner + strlen(allowed)),
	       "held fixture is the exact child-owned executable-side path");
	free(allowed);
	path = format("%s/fault.stderr", run->work.directory);
	diagnostic = readfile(path, MAX_JSON, &n);
	expect(strstr((const char *)diagnostic, owner) != NULL,
	       "refusal diagnostic names the exact retained fixture");
	free(diagnostic);
	free(path);
	entries = 0;
	check(SDL_EnumerateDirectory(owner, count_entry, &entries),
	      "inspect exact refused-removal fixture");
	expect(entries == 1, "held fixture contains only the owned payload");
	path = format("%s/held", owner);
	bytes = readfile(path, 1, &n);
	expect(n == 1 && bytes[0] == 'x', "held fixture payload unchanged");
	free(bytes);
	check(SDL_RemovePath(path), "remove exact exited child's payload");
	check(SDL_RemovePath(owner), "remove exact exited child's fixture");
	expect(!exists(path) && !exists(owner),
	       "parent removes only the checked fixture after child exits");
	free(path);
	SDL_free(output);
#else
	(void)run;
	puts("unit: held cross exit SKIP, requires Windows delete sharing");
#endif
}

static void
cross_cache_tests(App *run, const char *bullet, const char *source,
		  const char *original, const char *test_font)
{
	OutputWork *work = &run->work;
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

	expect(atexit(cleanup_cross_fixture) == 0,
	       "register cross-filesystem fixture cleanup");
	input_id = filesystem_id(work->directory);
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
	dir = fixture(run, "cross-chat", NULL);
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
	run_case(run, 1, bullet, "render", source, chat, "--output", output,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	run_case(run, 1, "ffmpeg", "-v", "error", "-xerror", "-i", output,
		 "-f", "null", "-", NULL);
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
	run_case(run, 1, bullet, "render", source, chat, "--output", output,
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
	reject_case(run, "external command failed", bullet, "render", source,
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
	check(end_cross_fixture(), "clean cross-filesystem fixture");
}

static void
cli_tests(App *run, const char *bullet, const char *tool)
{
	Renderer *renderer = &run->renderer;
	OutputWork *work = &run->work;
	SDL_Process **child = &run->child;
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
	SDL_Surface *png = NULL;
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
	beginwork(work, destination);
	free(destination);
	source = fixture(run, "元動画 ' & (source).mp4", NULL);
	original = fixture(run, "original.mp4", NULL);
	chat = fixture(run, "replay.data", twitch);
	result = fixture(run, "result.mp4", NULL);
	saved = fixture(run, "saved.mp4", NULL);
	invalid = fixture(run, "invalid.mp4", NULL);
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
	fake_bin = fixture(run, "fake-bin", NULL);
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
	run_case(run, 1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		 "testsrc2=s=160x90:r=30000/1001", "-f", "lavfi", "-i",
		 "sine=frequency=1000:sample_rate=48000", "-t", "2", "-c:v",
		 "libx264", "-g", "12", "-preset", "ultrafast", "-c:a", "aac",
		 "-movflags", "+faststart", source, NULL);
	copyfile(source, original);
	cross_cache_tests(run, bullet, source, original, test_font);
	run_case(run, 1, bullet, "--help", NULL);
	text = capture(child, version);
	expect(!strcmp(text, "bullet 0.1.0\n") ||
		   !strcmp(text, "bullet 0.1.0\r\n"),
	       "CLI reports the release version");
	free(text);
	auto_video = fixture(run, "auto.mp4", NULL);
	auto_chat = fixture(run, "auto.chat.json", twitch);
	auto_output = fixture(run, "auto.bullet.mp4", NULL);
	second_chat = fixture(run, "auto.live_chat.json", NULL);
	copyfile(source, auto_video);
	run_case(run, 1, bullet, "render", auto_video, "--font", test_font,
		 "--duration", "0.3", NULL);
	run_case(run, 1, "ffmpeg", "-v", "error", "-xerror", "-i", auto_output,
		 "-f", "null", "-", NULL);
	reject_case(run, "output exists", bullet, "render", auto_video,
		    "--font", test_font, NULL);
	writefile(second_chat, youtube, strlen(youtube));
	reject_case(run, "both chat formats exist", bullet, "render",
		    auto_video, "--font", test_font, "--force", NULL);
	run_case(run, 1, bullet, "render", auto_video, auto_chat, "--duration",
		 "0.3", "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(auto_chat), "remove mock Twitch chat");
	run_case(run, 1, bullet, "render", auto_video, "--duration", "0.3",
		 "--force", "--font", test_font, NULL);
	check(SDL_RemovePath(second_chat), "remove mock YouTube chat");
	reject_case(run, "no chat beside VIDEO", bullet, "render", auto_video,
		    "--force", NULL);
	samebytes(auto_video, source);
	free(auto_video);
	free(auto_chat);
	free(auto_output);
	free(second_chat);
	path = fixture(run, "sparse.data", sparse);
	/* Prefix/suffix trimming, internal gaps, nonzero crop origin, no
	 * sampled chat, sub-frame duration, scaling and rational clocks. */
	render_equal(run, bullet, tool, source, path, test_font, "0", "2",
		     "0.3", "50", "outline", "30000/1001", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0.501",
		     "0.3", "0.3", "100", "shadow", "25", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0.8", "0.2",
		     "0.3", "50", "outline", "30000/1001", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0", "0.1",
		     "0.3", "0", "shadow", "30000/1001", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0", "2",
		     "0.001", "50", "outline", "25", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0.501",
		     "0.001", "0.3", "50", "outline", "113394000/3780913",
		     "90");
	render_equal(run, bullet, tool, source, path, test_font, "0.05", "1.8",
		     "0.3", "50", "shadow", "113394000/3780913", "180");
	free(path);
	reject_case(run, "no chat beside VIDEO", bullet, "render", original,
		    "--output", result, NULL);
	run_case(run, 1, bullet, "render", source, chat, "--output", result,
		 "--font", test_font, NULL);
	run_case(run, 1, "ffmpeg", "-v", "error", "-xerror", "-i", result,
		 "-f", "null", "-", NULL);
	v = probe(child, result);
	expect(v.fps_num == 30000 && v.fps_den == 1001, "CLI rational fps");
	audio_equal(run, source, result);
	copyfile(result, saved);
	reject_case(run, "output exists", bullet, "render", source, chat,
		    "--output", result, NULL);
	samebytes(result, saved);
	run_case(run, 1, bullet, "render", source, chat, "--output", result,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	reject_case(run, "output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", source, "--force",
		    NULL);
	reject_case(run, "output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", chat, "--force", NULL);
	alias = fixture(run, "hardlink.mp4", NULL);
#ifdef _WIN32
	wa = wide(alias);
	wb = wide(source);
	expect(CreateHardLinkW(wa, wb, NULL), "create test hardlink");
	SDL_free(wa);
	SDL_free(wb);
#else
	expect(link(source, alias) == 0, "create test hardlink");
#endif
	reject_case(run, "output cannot be the source video or chat", bullet,
		    "render", source, chat, "--output", alias, "--force",
		    NULL);
	samebytes(source, alias);
	free(alias);

	backup = fixture(run, "new.backup.mp4", NULL);
	copyfile(source, backup);
	log = fixture(run, "new.ffmpeg.log", "untouched log");
	partial = fixture(run, "new.part.mp4", "untouched partial");
	other = fixture(run, "new.mp4", NULL);
	run_case(run, 1, bullet, "render", backup, chat, "--output", other,
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
	run_case(run, 1, bullet, "render", source, log, "--output", other,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	samebytes(log, chat);
	samebytes(backup, original);
	free(backup);
	free(log);
	free(partial);
	free(other);

	broken = fixture(run, "broken.mp4", NULL);
	data = readfile(source, MAX_JSON, &n);
	writefile(broken, data, n * 2 / 3);
	free(data);
	probe(child, broken);
	run_case(run, 0, "ffmpeg", "-v", "error", "-xerror", "-i", broken,
		 "-f", "null", "-", NULL);
	copyfile(result, saved);
	/* Decoder failure can reach the pipe write or the process wait.
	 * Require the FFmpeg phase, not one timing-dependent symptom. */
	reject_case(run, "FFmpeg log:", bullet, "render", broken, chat,
		    "--output", result, "--force", "--font", test_font, NULL);
	samebytes(result, saved);
	free(broken);
	for (i = 0; i < sizeof bad_options / sizeof *bad_options; i++)
		reject_case(run, bad_options[i].error, bullet, "render",
			    source, chat, "--output", invalid,
			    bad_options[i].key, bad_options[i].value, NULL);
	for (i = 0; i < sizeof invalid_aspects / sizeof *invalid_aspects; i++)
		reject_case(run, "emote count or aspect ratio exceeds limit",
			    tool, "--invalid-duplicate", invalid_aspects[i],
			    NULL);
	path = fixture(run, "bad.json", NULL);
	for (i = 0; i < sizeof bad_json / sizeof *bad_json; i++) {
		writefile(path, bad_json[i].text, strlen(bad_json[i].text));
		reject_case(run, bad_json[i].error, bullet, "render", source,
			    path, "--output", invalid, "--font", test_font,
			    NULL);
	}
	/* Include the terminator: a valid JSON prefix must not hide NUL. */
	writefile(path, twitch, strlen(twitch) + 1);
	reject_case(run, "NUL byte in JSON input", bullet, "render", source,
		    path, "--output", invalid, "--font", test_font, NULL);
	free(path);
	expect(!exists(invalid), "failed render does not publish an output");
	run_case(run, 1, bullet, "render", source, chat, "--output", result,
		 "--force", "--duration", "1", "--hls-start",
		 "2026-01-01T00:00:00Z", "--font", test_font, NULL);
	portrait = fixture(run, "portrait.mp4", NULL);
	run_case(run, 1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		 "color=s=144x256:r=12", "-t", "1", "-vf", "setsar=4/3",
		 "-c:v", "mpeg4", portrait, NULL);
	run_case(run, 1, bullet, "render", portrait, chat, "--output", result,
		 "--force", "--duration", "0.5", "--font", test_font, NULL);
	v = probe(child, result);
	expect(v.width == 192 && v.height == 256, "portrait and SAR");
	free(portrait);

	/* All four image codecs and mixed animated/static rendering, offline.
	 */
	dir = fixture(run, "assets", NULL);
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
	surface_create(renderer, &png, 2, 2);
	memset(png->pixels, 255, (size_t)png->pitch * png->h);
	io = SDL_IOFromFile(image, "wb");
	expect(io != NULL && IMG_SavePNG_IO(png, io, true), "PNG fixture");
	surface_destroy(renderer, &png);
	path = fixture(run, "mixed.json", mixed);
	run_case(run, 1, bullet, "render", source, path, "--output", result,
		 "--force", "--duration", "1", "--travel-time", "1", "--font",
		 test_font, NULL);
	run_case(run, 1, "ffmpeg", "-v", "error", "-xerror", "-i", result,
		 "-f", "null", "-", NULL);
	render_equal(run, bullet, tool, source, path, test_font, "0", "2",
		     "0.5", "50", "outline", "30000/1001", "90");
	render_equal(run, bullet, tool, source, path, test_font, "0.1", "1.8",
		     "0.5", "100", "shadow", "25", "90");
	for (i = 0; i < 2; i++) {
		run_case(run, 1, "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
			 "color=c=red:s=8x8", "-frames:v", "1", "-c:v",
			 i ? "libwebp" : "mjpeg", "-f", "image2", "-update",
			 "1", "-y", image, NULL);
		run_case(run, 1, bullet, "render", source, path, "--output",
			 result, "--force", "--duration", "0.4", "--font",
			 test_font, NULL);
	}
	free(image);
	free(path);
	text = format("{\"comments\":[{\"content_offset_seconds\":0,"
		      "\"message\":{\"fragments\":[{\"emoticon\":{"
		      "\"emoticon_id\":\"broken\"}}]}}],\"embeddedData\":{"
		      "\"firstParty\":[{"
		      "\"id\":\"broken\",\"data\":\"%.*s\"}]}}",
		      (int)strlen(gif) - 16, gif);
	path = fixture(run, "bad-gif.json", text);
	free(text);
	run_case(run, 1, bullet, "render", source, path, "--output", result,
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
	reject_case(run, "external command failed", bullet, "render", source,
		    path, "--output", result, "--force", "--font", test_font,
		    NULL);
	samebytes(result, saved);
	free(path);
	hashurl("https://static-cdn.jtvnw.net/emoticons/v2/broken/default/"
		"dark/2.0",
		hash);
	path = format("%s/%s", dir, hash);
	expect(!exists(path), "broken GIF never becomes a cached image");
	free(path);
	free(dir);

	yt = fixture(run, "youtube.data", youtube);
	tools = fixture(run, "tools.log", NULL);
	test_env("YT_DLP", tool);
	test_env("TWITCH_DOWNLOADER_CLI", tool);
	test_env("BULLET_FIXTURE_VIDEO", source);
	test_env("BULLET_FIXTURE_CHAT", chat);
	test_env("BULLET_FIXTURE_YOUTUBE", yt);
	test_env("BULLET_TOOL_LOG", tools);
	dir = fixture(run, "youtube", NULL);
	run_case(run, 1, bullet, "download",
		 "https://www.youtube.com/watch?v=fixture", "--dir", dir,
		 NULL);
	path = format("%s/fixture.mp4", dir);
	other = format("%s/fixture.live_chat.json", dir);
	samebytes(path, source);
	run_case(run, 1, bullet, "render", path, other, "--output", result,
		 "--force", "--duration", "0.3", "--font", test_font, NULL);
	free(path);
	free(other);
	free(dir);
	dir = fixture(run, "twitch", NULL);
	run_case(run, 1, bullet, "download",
		 "https://www.twitch.tv/videos/123", "--dir", dir, NULL);
	path = format("%s/v123.mp4", dir);
	cached = fixture(run, "tools-before.log", NULL);
	copyfile(tools, cached);
	run_case(run, 1, bullet, "download",
		 "https://www.twitch.tv/videos/123", "--dir", dir, NULL);
	samebytes(tools, cached);
	samebytes(path, source);
	writefile(path, "truncated", 9);
	reject_case(run, "external command failed", bullet, "download",
		    "https://www.twitch.tv/videos/123", "--dir", dir, NULL);
	samebytes(tools, cached);
	before_entries = 0;
	check(SDL_EnumerateDirectory(dir, count_entry, &before_entries),
	      "count files before failed download");
	test_env("BULLET_TOOL_FAIL", "1");
	reject_case(run, "test downloader failed", bullet, "download",
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
	other = fixture(run, "truncated-before.mp4", "truncated");
	samebytes(path, other);
	free(other);
	test_env("BULLET_TOOL_FAIL", "");
	reject_case(run, "expected a public HTTPS archive URL", bullet,
		    "download", "http://www.youtube.com/watch?v=x", "--dir",
		    dir, NULL);
	reject_case(run, "only YouTube and Twitch archive URLs are supported",
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
	reject_case(run, "write failed", bullet, "render", source, chat,
		    "--output", result, "--force", "--font", test_font, NULL);
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
	printf("cli: %u native process checks passed; inputs unchanged\n",
	       cli_checks);
}

static void
fixture_empty(const App *run)
{
	expect(
	    !run->chat.messages && !run->chat.assets && !run->chat.nmessages &&
		!run->chat.nassets && !run->chat.nparts,
	    "fixture Chat arrays, parts, strings, frames and sprites empty");
	expect(!run->glyphs.font,
	       "fixture font owner empty after native case");
	expect(!run->renderer.canvas, "fixture canvas owner empty");
	expect(!run->work.directory && !run->work.stage &&
		   !run->work.asset_temp && !run->work.logpath &&
		   !run->work.log,
	       "fixture work paths and log owners empty");
	expect(!run->cache_stage.directory && !run->cache_stage.payload,
	       "fixture cache stage owners empty");
	expect(!run->child && !run->inferred_chat && !run->inferred_output &&
		   !run->chat_context && !cross_fixture &&
		   !frame_storage_source,
	       "fixture process, inferred paths and test owners empty");
	expect(run->renderer.retained.bytes == 0,
	       "fixture retained image storage reaches zero without reset");
}

static void
fixture_begin(App *run)
{
	Options defaults = {0};
	Glyphs empty_glyphs = {0};
	RenderPlan empty_plan = {0};

	fixture_empty(run);
	defaults.duration = -1;
	defaults.travel = default_travel;
	defaults.opacity = default_opacity;
	defaults.dir = "data";
	defaults.max_height = 720;
	run->options = defaults;
	run->glyphs = empty_glyphs;
	run->renderer.plan = empty_plan;
	run->renderer.dense_reference = 0;
	run->work.keep_log = 0;
	run->fail_next_resize = run->fail_embedded_replacement = 0;
}

static void
fixture_ready(const App *run)
{
	const Options *o = &run->options;
	const RenderPlan *p = &run->renderer.plan;
	const Glyphs *g = &run->glyphs;

	fixture_empty(run);
	expect(!o->video && !o->chat && !o->output && !o->font && !o->url &&
		   o->dir && !strcmp(o->dir, "data") && !o->start &&
		   o->duration == -1 && o->travel == default_travel &&
		   !o->origin && !o->fps_num && !o->fps_den && !o->height &&
		   o->opacity == default_opacity && !o->shadow && !o->force &&
		   !o->hls && o->max_height == 720,
	       "independent case has default options");
	expect(!p->width && !p->height && !p->fps_num && !p->fps_den &&
		   !p->start && !p->duration && !p->travel && !p->opacity &&
		   !p->shadow && !g->font_size && !g->outline && !g->gap &&
		   !g->lane_height && !g->emote_height &&
		   !run->renderer.dense_reference && !run->work.keep_log &&
		   !run->fail_next_resize && !run->fail_embedded_replacement,
	       "independent case has empty plan, metrics and failure hooks");
}

static void
fixture_end(App *run)
{
	int status;
	char *directory =
	    run->work.directory ? copystr(run->work.directory) : NULL;

	if (run->child) {
		SDL_KillProcess(run->child, true);
		check(SDL_WaitProcess(run->child, true, &status),
		      "wait for fixture-owned child");
		SDL_DestroyProcess(run->child);
		run->child = NULL;
	}
	if (run->work.log) {
		status = SDL_CloseIO(run->work.log);
		run->work.log = NULL;
		check(status, "close fixture-owned log");
	}
	check(end_cache_stage(&run->cache_stage),
	      "release fixture cache stage");
	if (run->work.directory)
		check(SDL_EnumerateDirectory(run->work.directory, remove_entry,
					     NULL),
		      "remove only exclusively owned fixture contents");
	endwork(&run->work, &run->cache_stage);
	check(end_cross_fixture(), "release owned cross fixture");
	expect(freechat(&run->chat, &run->renderer),
	       "release every fixture string, frame and sprite");
	expect(surface_destroy(&run->renderer, &run->renderer.canvas),
	       "release fixture canvas charge");
	if (run->glyphs.font) {
		TTF_CloseFont(run->glyphs.font);
		run->glyphs.font = NULL;
		TTF_Quit();
	}
	free(run->inferred_chat);
	free(run->inferred_output);
	run->inferred_chat = run->inferred_output = NULL;
	fixture_empty(run);
	if (directory) {
		expect(!exists(directory), "fixture work directory removed");
		free(directory);
	}
}

static void
fixture_ownership_tests(App *run)
{
	Message *m;
	Asset *a;
	SDL_Surface *source;
	const char *args[] = {unit_program, "--version", NULL};
	int status;

	unit_work(run);
	run->work.stage = format("%s/stage", run->work.directory);
	writefile(run->work.stage, "stage", 5);
	writefile(run->work.asset_temp, "encoded", 7);
	run->work.log = SDL_IOFromFile(run->work.logpath, "wb");
	check(run->work.log != NULL, "create owned boundary log");
	run->cache_stage.directory = private_directory(run->work.directory);
	run->cache_stage.payload =
	    format("%s/payload", run->cache_stage.directory);
	writefile(run->cache_stage.payload, "cache", 5);
	m = message(&run->chat, 123456);
	part(&run->chat, m, "owned boundary text", NONE);
	m->sprite = resize(NULL, 1, sizeof *m->sprite);
	memset(m->sprite, 0, sizeof *m->sprite);
	surface_create(&run->renderer, &m->sprite->pixels, 8, 2);
	((unsigned char *)m->sprite->pixels->pixels)[3] = 255;
	sprite_pack(&run->renderer, m->sprite);
	a = &run->chat.assets[asset(&run->chat,
				    "https://example.com/boundary.png", 1)];
	a->embedded = copystr("owned boundary encoded bytes");
	a->target_width = 2;
	source = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
	check(source != NULL, "create boundary source");
	asset_frame(&run->renderer, 2, a, source, 100);
	SDL_DestroySurface(source);
	surface_create(&run->renderer, &run->renderer.canvas, 2, 2);
	openfont(&run->glyphs, SDL_getenv("BULLET_TEST_FONT"), 200, 80);
	run->inferred_chat = copystr("owned inferred chat");
	run->inferred_output = copystr("owned inferred output");
	run->options.chat = run->inferred_chat;
	run->options.output = run->inferred_output;
	run->options.hls = 1;
	run->options.origin = 654321;
	run->renderer.plan.start = 123456;
	run->renderer.plan.travel = 987654;
	run->renderer.dense_reference = 1;
	spawn(&run->child, args, 0, 0, run->work.log);
	check(SDL_WaitProcess(run->child, true, &status),
	      "wait for boundary child without destroying its owner");
	expect(status == 0 && run->chat.nmessages == 1 &&
		   run->chat.nparts == 1 && run->chat.nassets == 1 &&
		   m->sprite->runs && !m->sprite->pixels && a->count == 1 &&
		   run->renderer.retained.bytes == 48 && run->glyphs.font &&
		   run->child && run->work.log,
	       "boundary owns literal text, packed sprite, frame, canvas, "
	       "font, process, log and paths before real cleanup");
}

typedef struct {
	const char *name;
	void (*callback)(App *run);
} UnitCase;

static const UnitCase unit_cases[] = {
    {"fixture-ownership", fixture_ownership_tests},
    {"embedded-replacement", replacement_tests},
    {"retained-storage", storage_tests},
    {"alpha-pairs", blend_tests},
    {"packed-sprites", sprite_tests},
    {"subpixel-motion", subpixel_motion_tests},
    {"frame-bounds", frame_bounds_tests},
    {"numeric-lanes", lane_tests},
    {"immutable-plan", plan_tests},
    {"visible-replanning", replan_tests},
    {"width-geometry", width_geometry_tests},
    {"asset-metadata", asset_metadata_tests},
    {"asset-frame-time", asset_frame_tests},
    {"clock-hash", clock_hash_tests},
    {"straight-alpha", alpha_tests},
    {"chat-clocks", chat_clock_tests},
    {"width-decode", width_decode_tests},
    {"animated-render-cache", animated_render_tests},
    {"text-lanes", text_lane_tests},
    {"held-cache-cleanup", cache_cleanup_tests},
    {"held-cross-cleanup", cross_cleanup_retry},
    {"replacement-oom", replacement_failure_tests},
    {"frame-storage-oom", storage_failure_tests},
    {"release-mismatch", release_failure_tests},
    {"held-cross-exit", held_exit_tests}};

static void
unit_run(App *run, const UnitCase *test, unsigned int repetition)
{
	printf("unit: begin %s repetition %u\n", test->name, repetition);
	fflush(stdout);
	fixture_begin(run);
	fixture_ready(run);
	test->callback(run);
	fixture_end(run);
	fixture_empty(run);
	printf("unit: end %s repetition %u, all owners empty, retained 0\n",
	       test->name, repetition);
}

static void
unit_named(App *run, const char *name)
{
	size_t i;

	for (i = 0; i < sizeof unit_cases / sizeof *unit_cases; i++)
		if (!strcmp(unit_cases[i].name, name)) {
			unit_run(run, &unit_cases[i], 1);
			return;
		}
	die("unknown native case: %s", name);
}

static void
unit_tests(App *run, int reverse, unsigned int repetitions)
{
	size_t i, count = sizeof unit_cases / sizeof *unit_cases;
	unsigned int repetition;

	for (repetition = 1; repetition <= repetitions; repetition++)
		for (i = 0; i < count; i++)
			unit_run(run, &unit_cases[reverse ? count - 1 - i : i],
				 repetition);
	printf("unit: %zu independent cases, %s, %u repetitions passed\n",
	       count, reverse ? "reverse" : "forward", repetitions);
}

int
main(int argc, char **argv)
{
	App *run = &app;
	int mode;

	atexit(cleanup);
	check(SDL_Init(0), "initialize SDL");
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		die("initialize HTTP library");
	unit_program = argv[0];
	if (argc > 1) {
		if (argc == 3 && !strcmp(argv[1], "--unit-case")) {
			unit_named(run, argv[2]);
			return 0;
		}
		if (argc == 3 && !strcmp(argv[1], "--unit-order")) {
			expect(!strcmp(argv[2], "forward") ||
				   !strcmp(argv[2], "reverse"),
			       "native order is forward or reverse");
			unit_tests(run, !strcmp(argv[2], "reverse"), 1);
			return 0;
		}
		if (argc == 2 && !strcmp(argv[1], "--unit-repeat")) {
			unit_tests(run, 0, 2);
			unit_tests(run, 1, 2);
			return 0;
		}
		if (SDL_getenv("BULLET_REFERENCE_ENCODER") &&
		    *SDL_getenv("BULLET_REFERENCE_ENCODER") &&
		    (!SDL_strcasecmp(basenameof(argv[0]), "ffmpeg") ||
		     !SDL_strcasecmp(basenameof(argv[0]), "ffmpeg.exe"))) {
			reference_ffmpeg(run, argc, argv);
			return 0;
		}
		if (argc == 3 && !strcmp(argv[1], "--invalid-duplicate")) {
			asset(&app.chat, "https://example.com/duplicate.png",
			      2);
			asset(&app.chat, "https://example.com/duplicate.png",
			      strtod(argv[2], NULL));
			freechat(&app.chat, &app.renderer);
			return 0;
		}
		if (!strcmp(argv[1], "--storage")) {
			unit_named(run, "retained-storage");
			return 0;
		}
		if (argc == 5 && !strcmp(argv[1], "--release-mismatch"))
			release_mismatch(run, !strcmp(argv[2], "packed"),
					 !strcmp(argv[3], "invalid"),
					 !strcmp(argv[4], "atexit"));
		if (!strcmp(argv[1], "--frame-storage-oom"))
			frame_storage_oom(run, argc == 3 ? atoi(argv[2]) : 1);
		if (!strcmp(argv[1], "--replacement-oom"))
			replacement_oom(run);
		if (!strcmp(argv[1], "--held-cross-exit"))
			held_cross_exit();
		if (!strcmp(argv[1], "--cross-cleanup-retry")) {
			unit_named(run, "held-cross-cleanup");
			return 0;
		}
		if (!strcmp(argv[1], "--owners")) {
			unit_named(run, "numeric-lanes");
			unit_named(run, "immutable-plan");
			unit_named(run, "visible-replanning");
			return 0;
		}
		if (!strcmp(argv[1], "--replan")) {
			unit_named(run, "visible-replanning");
			return 0;
		}
		if (!strcmp(argv[1], "--width-geometry")) {
			unit_named(run, "width-geometry");
			return 0;
		}
		if (!strcmp(argv[1], "--version")) {
			puts("bullet offline test tool");
			return 0;
		}
		if (argc == 3 && !strcmp(argv[1], "--cli")) {
			fixture_begin(run);
			fixture_ready(run);
			cli_tests(run, argv[2], argv[0]);
			fixture_end(run);
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
			app.renderer.dense_reference = 1;
			argv[1] = "render";
		}
		mode = arguments(&app.options, &app.inferred_chat,
				 &app.inferred_output, argc, argv);
		if (mode == 1)
			download(&app.options, &app.chat, &app.renderer,
				 &app.work, &app.cache_stage, &app.child);
		else
			render(&app.options, &app.chat, &app.glyphs,
			       &app.renderer, &app.work, &app.cache_stage,
			       &app.child);
		return 0;
	}
	unit_tests(run, 0, 1);
	return 0;
}
