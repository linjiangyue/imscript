// tiff utils //
// info   f.tiff            # print misc info
// ntiles f.tiff            # print the total number of tiles
// tget   f.tiff n t.tiff   # get the nth tile (sizes must coincide)
// tput   f.tiff n t.tiff   # an image into the nth tile (sizes must coincide)
// tzout  a.tiff b.tiff     # zoom out by a factor 2 (keeping tile size)
// retile in.t tw th out.t
// tzero  w h tw th spp bps fmt
//
// meta   prog in.tiff ... out.tiff # run "prog" for all the tiles


#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <tiffio.h>

#include "fail.c"
#include "xmalloc.c"


struct tiff_tile {
	int32_t w, h;
	int16_t spp, bps, fmt;
	bool broken;
	uint8_t *data;

	int offx, offy;
};

struct tiff_info {
	int32_t w;   // image width
	int32_t h;   // image height
	int16_t spp; // samples per pixel
	int16_t bps; // bits per sample
	int16_t fmt; // sample format
	bool broken; // whether pixels are contiguous or broken
	bool packed; // whether bps=1,2 or 4
	bool tiled;  // whether data is organized into tiles
	bool compressed;
	int ntiles;

	// only if tiled
	int32_t tw;  // tile width
	int32_t th;  // tile height
	int32_t ta;  // tiles across
	int32_t td;  // tiles down
};

static int tinfo_pixelsize(struct tiff_info *t)
{
	if (t->packed)
		fail("packed bad");
	return t->spp * t->bps / 8;
}

static int tinfo_tilesize(struct tiff_info *t)
{
	//if (!t->tiled || t->broken || t->packed || t->compressed)
	if (!t->tiled || t->packed)
		fail("this combination is not supported");
	return t->tw * t->th * t->spp * (t->bps/8);
}

// tell how many units "u" are needed to cover a length "n"
static int how_many(int n, int u)
{
	assert(n > 0);
	assert(u > 0);
	return n/u + (bool)(n%u);

}

static int fmt_from_string(char *f)
{
	if (0 == strcmp(f, "uint")) return SAMPLEFORMAT_UINT;
	if (0 == strcmp(f, "int")) return SAMPLEFORMAT_INT;
	if (0 == strcmp(f, "ieeefp")) return SAMPLEFORMAT_IEEEFP;
	//if (0 == strcmp(f, "void")) return SAMPLEFORMAT_VOID;
	if (0 == strcmp(f, "complexint")) return SAMPLEFORMAT_COMPLEXINT;
	if (0 == strcmp(f, "complexieeefp")) return SAMPLEFORMAT_COMPLEXIEEEFP;
	return SAMPLEFORMAT_VOID;
}

static char *fmt_to_string(int fmt)
{
	switch(fmt) {
	case SAMPLEFORMAT_UINT: return "uint";
	case SAMPLEFORMAT_INT: return "int";
	case SAMPLEFORMAT_IEEEFP: return "ieeefp";
	case SAMPLEFORMAT_COMPLEXINT: return "complexint";
	case SAMPLEFORMAT_COMPLEXIEEEFP: return "complexieeefp";
	}
	return "unrecognized";
}


static void get_tiff_info(struct tiff_info *t, TIFF *tif)
{
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEWIDTH,      &t->w);
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGELENGTH,     &t->h);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &t->spp);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE,   &t->bps);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,    &t->fmt);

	uint16_t planarity, compression;
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG,    &planarity);
	TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION,     &compression);
	t->broken = planarity != PLANARCONFIG_CONTIG;
	t->compressed = compression != COMPRESSION_NONE;
	t->packed = 0 != t->bps % 8;
	t->tiled = TIFFIsTiled(tif);

	if (t->tiled) {
		TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &t->tw);
		TIFFGetField(tif, TIFFTAG_TILELENGTH, &t->th);
		t->ta = how_many(t->w, t->tw);
		t->td = how_many(t->h, t->th);
		t->ntiles = TIFFNumberOfTiles(tif);
	}
}

static void get_tiff_info_filename(struct tiff_info *t, char *fname)
{
	TIFF *tif = TIFFOpen(fname, "r");
	if (!tif)
		fail("could not open TIFF file \"%s\" for reading", fname);
	get_tiff_info(t, tif);
	TIFFClose(tif);
}

static int tiff_imagewidth(TIFF *tif)
{
	uint32_t w, r = TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	if (r != 1) fail("can not read TIFFTAG_IMAGEWIDTH");
	return w;
}

static int tiff_imagelength(TIFF *tif)
{
	uint32_t h, r = TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
	if (r != 1) fail("can not read TIFFTAG_IMAGELENGTH");
	return h;
}

static int tiff_samplesperpixel(TIFF *tif)
{
	uint16_t spp;
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
	return spp;
}

// divide by 8 to obtain the size per sample (then, 0=packed data)
static int tiff_bitspersample(TIFF *tif)
{
	uint16_t bps;
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
	return bps;
}

static int tiff_sampleformat(TIFF *tif)
{
	uint16_t fmt;
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
	return fmt;
}

static bool tiff_brokenpixels(TIFF *tif)
{
	uint16_t planarity;
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planarity);
	return planarity != PLANARCONFIG_CONTIG;
}

static int tiff_tilewidth(TIFF *tif)
{
	uint32_t R;
	int r = TIFFGetField(tif, TIFFTAG_TILEWIDTH, &R);
	if (r != 1) fail("TIFF tiles of unknown width");
	return R;
}

static int tiff_tilelength(TIFF *tif)
{
	uint32_t R;
	int r = TIFFGetField(tif, TIFFTAG_TILELENGTH, &R);
	if (r != 1) fail("TIFF tiles of unknown width");
	return R;
}

static int tiff_tilesacross(TIFF *tif)
{
	int w = tiff_imagewidth(tif);
	int tw = tiff_tilewidth(tif);
	return how_many(w, tw);
}

static int tiff_tilesdown(TIFF *tif)
{
	int h = tiff_imagelength(tif);
	int th = tiff_tilelength(tif);
	return how_many(h, th);
}

static int tiff_tile_corner(int p[2], TIFF *tif, int tidx)
{
	p[0] = p[1] = -1;

	int tw = tiff_tilewidth(tif);
	int th = tiff_tilelength(tif);
	int ta = tiff_tilesacross(tif);
	int td = tiff_tilesdown(tif);

	int i = tidx % ta;
	int j = tidx / ta;

	if (!(i < ta && j < td))
		return 0;

	p[0] = tw*i;
	p[1] = th*j;
	return 1;
}



// write a non-tiled TIFF image
static void write_tile_to_file(char *filename, struct tiff_tile *t)
{
	if (t->broken) fail("can not save broken tiles yet");

	TIFF *tif = TIFFOpen(filename, "w");
	if (!tif) fail("could not open TIFF file \"%s\" for writing", filename);

	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      t->w);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     t->h);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, t->spp);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   t->bps);
	TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,    t->fmt);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);

	int Bps = t->bps/8;
	if (!Bps) fail("packed pixels not supported yet");

	int scanline_size = t->w * t->spp * Bps;
	for (int i = 0; i < t->h; i++)
	{
		void *line = i*scanline_size + t->data;
		int r = TIFFWriteScanline(tif, line, i, 0);
		if (r < 0) fail("error writing %dth TIFF scanline", i);
	}

	TIFFClose(tif);
}

// overwrite a tile on an existing tiled TIFF image
static void put_tile_into_file(char *filename, struct tiff_tile *t, int tidx)
{
	if (t->broken) fail("can not save broken tiles yet");

	// Note, the mode "r+" is officially undocumented, but its behaviour is
	// described on file tif_open.c from libtiff.
	TIFF *tif = TIFFOpen(filename, "r+");
	if (!tif) fail("could not open TIFF file \"%s\" for writing", filename);

	int tw = tiff_tilewidth(tif);
	int th = tiff_tilewidth(tif);
	int spp = tiff_samplesperpixel(tif);
	int bps = tiff_bitspersample(tif);
	int fmt = tiff_sampleformat(tif);
	if (tw != t->w) fail("tw=%d different to t->w=%d", tw, t->w);
	if (th != t->h) fail("th=%d different to t->h=%d", th, t->h);
	if (spp != t->spp) fail("spp=%d different to t->spp=%d", spp, t->spp);
	if (bps != t->bps) fail("bps=%d different to t->bps=%d", bps, t->bps);
	if (spp != t->spp) fail("spp=%d different to t->spp=%d", spp, t->spp);

	int ii[2];
	int r = tiff_tile_corner(ii, tif, tidx);
	if (!r) fail("bad tile %d", tidx);

	r = TIFFWriteTile(tif, t->data, ii[0], ii[1], 0, 0);

	TIFFClose(tif);
}

static void dalloc_tile_from_open_file(struct tiff_tile *t, TIFF *tif, int tidx)
{
	//struct tiff_info tinfo[1];
	//get_tiff_info(tinfo, tif);

	//int nt = TIFFNumberOfTiles(tif);
	//if (tidx < 0 || tidx >= nt)
	//	fail("bad tile index %d", tidx);

	//t->w = tinfo->tw;
	//t->h = tinfo->th;
	//t->spp = tinfo->spp;
	//t->bps = tinfo->bps;
	//t->fmt = tinfo->fmt;
	//t->broken = false;

	//int tbytes = TIFF

	//int i = tidx % ta;
	//int j = tidx / ta;

	//int ii = tw*i;
	//int jj = th*i;

	//int tbytes = TIFFTileSize(tif);
	//t->data = xmalloc(tbytes);
	//memset(t->data, 0, tbytes);
	//int r = TIFFReadTile(tif, t->data, ii, jj, 0, 0);
	//if (r != tbytes) fail("could not read tile");

}

static void read_scanlines(struct tiff_tile *tout, TIFF *tif)
{
	// read all file info
	struct tiff_info tinfo[1];
	get_tiff_info(tinfo, tif);

	// fill-in output information
	tout->w = tinfo->w;
	tout->h = tinfo->h;
	tout->bps = tinfo->bps;
	tout->fmt = tinfo->fmt;
	tout->spp = tinfo->spp;

	// define useful constants
	int pixel_size = tinfo->spp * tinfo->bps/8;
	int output_size = tout->w * tout->h * pixel_size;

	// allocate space for output data
	tout->data = xmalloc(output_size);

	// copy scanlines
	int scanline_size = TIFFScanlineSize(tif);
	assert(scanline_size == tinfo->w * pixel_size);
	for (int j = 0; j < tinfo->h; j++)
	{
		uint8_t *buf = tout->data + scanline_size*j;
		int r = TIFFReadScanline(tif, buf, j, 0);
		if (r < 0) fail("could not read scanline %d", j);
	}
}

static tsize_t my_readtile(TIFF *tif, tdata_t buf,
		uint32 x, uint32 y, uint32 z, tsample_t sample)
{
	tsize_t r = TIFFReadTile(tif, buf, x, y, z, sample);
	if (r == -1) memset(buf, 0, r = TIFFTileSize(tif));
	return r;
}

static void read_tile_from_file(struct tiff_tile *t, char *filename, int tidx)
{
	TIFF *tif = TIFFOpen(filename, "r");
	if (!tif) fail("could not open TIFF file \"%s\" for reading", filename);

	uint32_t w, h;
	uint16_t spp, bps, fmt, planarity;
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEWIDTH,      &w);
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGELENGTH,     &h);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE,   &bps);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,    &fmt);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG,    &planarity);
	t->spp = spp;
	t->bps = bps;
	t->fmt = fmt;
	t->broken = false;

	if (planarity != PLANARCONFIG_CONTIG)
		fail("broken pixels not supported yet");

	if (TIFFIsTiled(tif)) {
		int nt = TIFFNumberOfTiles(tif);
		if (tidx < 0 || tidx >= nt)
			fail("bad tile index %d", tidx);

		t->w = tiff_tilewidth(tif);;
		t->h = tiff_tilelength(tif);

		int ii[2];
		tiff_tile_corner(ii, tif, tidx);
		//int ii = tw*i;
		//int jj = th*j;

		int tbytes = TIFFTileSize(tif);
		t->data = xmalloc(tbytes);
		memset(t->data, 0, tbytes);
		int r = my_readtile(tif, t->data, ii[0], ii[1], 0, 0);
		if (r != tbytes) fail("could not read tile");
	} else { // not tiled, read the whole image into 0th tile
		read_scanlines(t, tif);
	}

	TIFFClose(tif);
}

// overwrite tile "idx" of the given file
static void insert_tile_into_file(char *filename, struct tiff_tile *t, int idx)
{
}

static void tiffu_whatever(char *filename)
{
	TIFF *tif = TIFFOpen(filename, "r");

	int w = tiff_imagewidth(tif);
	int h = tiff_imagelength(tif);
	int spp = tiff_samplesperpixel(tif);
	int Bps = tiff_bitspersample(tif)/8;
	int fmt = tiff_sampleformat(tif);

	if (TIFFIsTiled(tif)) {
		int tw = tiff_tilewidth(tif);
		int th = tiff_tilelength(tif);
		int ta = tiff_tilesacross(tif);
		int td = tiff_tilesdown(tif);
		int nt = TIFFNumberOfTiles(tif);
		bool broken = tiff_brokenpixels(tif);

		printf("TIFF w = %d\n", w);
		printf("TIFF h = %d\n", h);
		printf("TIFF spp = %d\n", spp);
		printf("TIFF Bps = %d\n", Bps);
		printf("TIFF fmt = %d\n", fmt);
		printf("TIFF broken pixels = %s\n", broken ? "yes" : "no");

		printf("TIFF tw = %d\n", tw);
		printf("TIFF th = %d\n", th);
		printf("TIFF ta = %d\n", ta);
		printf("TIFF td = %d\n", td);
		printf("TIFF ta * td = %d\n", ta * td);
		printf("TIFF nt = %d\n", nt);

		for (int j = 0; j < td; j++)
		for (int i = 0; i < ta; i++)
		{
			int ii = i*tw;
			int jj = j*th;
			int tidx = TIFFComputeTile(tif, ii, jj, 0, 0);
			fprintf(stderr, "%d %d : %d %d : %d (%d)\n",
					i, j, ii, jj, tidx, j*ta+i);
		}

		for (int t = 0; t < nt; t++)
		{
			int i = t % ta;
			int j = t / ta;
			int ii = i*tw;
			int jj = j*th;
			int tidx = TIFFComputeTile(tif, ii, jj, 0, 0);
			//fprintf(stderr, "t=%d i=%d j=%d tidx=%d\n",
			//		t, i, j, tidx);
			fprintf(stderr, "%d %d\n", t, tidx);
		}
	} else {
		printf("not tiled\n");
	}

	TIFFClose(tif);
}

static void tiffu_print_info(char *filename)
{
	TIFF *tif = TIFFOpen(filename, "r");
	if (!tif) fail("could not open TIFF file \"%s\"", filename);

	uint32_t w, h;
	uint16_t spp, bps, fmt;
	int r = 0;
	r += TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	r += TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
	if (r != 2) fail("can not treat TIFF of unkwnown size");
	printf("TIFF %dx%d\n", w, h);

	r = TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
	if(!r) spp=1;
	if(r) printf("TIFF spp %d (r=%d)\n", spp, r);

	r = TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
	if(!r) bps=1;
	if(r) printf("TIFF bps %d (r=%d)\n", spp, r);

	r = TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
	if(!r) fmt = SAMPLEFORMAT_UINT;
	if(r) printf("TIFF fmt %d (r=%d)\n", spp, r);

	if (TIFFIsTiled(tif)) {
		int tisize = TIFFTileSize(tif);
		uint32_t tilewidth, tilelength;
		r = 0;
		r += TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tilewidth);
		r += TIFFGetField(tif, TIFFTAG_TILELENGTH, &tilelength);
		if (r != 2) fail("TIFF tiles of unknown size");

		int ntiles = (w/tilewidth)*(h/tilelength);
		int ntiles2 = TIFFNumberOfTiles(tif);

		printf("TIFF ntiles %d (%d whole)\n", ntiles2, ntiles);
		printf("TIFF tilesize %d (%dx%d)\n", tisize,
				tilewidth, tilelength);

		int tiles_across = tiff_tilesacross(tif);
		int tiles_down = tiff_tilesdown(tif);
		int ntiles3 = tiles_across * tiles_down;

		printf("TIFF tile config = %dx%d (%d)\n",
				tiles_across, tiles_down, ntiles3);

	}

	TIFFClose(tif);
}

static int tiffu_ntiles(char *filename)
{
	int r = 1;

	TIFF *tif = TIFFOpen(filename, "r");
	if (!tif) fail("could not open TIFF file \"%s\"", filename);

	if (TIFFIsTiled(tif)) {
		uint32_t w, h, tw, tl;
		r += TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
		r += TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
		r += TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
		r += TIFFGetField(tif, TIFFTAG_TILELENGTH, &tl);
		if (r != 5) fail("TIFF tiles of unknown size");

		r = (w/tw)*(h/tl);
	}

	TIFFClose(tif);
	return r;
}

static int tiffu_ntiles2(char *filename)
{
	int r = 0;

	TIFF *tif = TIFFOpen(filename, "r");
	if (!tif) fail("could not open TIFF file \"%s\"", filename);

	if (TIFFIsTiled(tif)) {
		r = TIFFNumberOfTiles(tif);
	}

	TIFFClose(tif);
	return r;
}

// get a tile (low level: output image is filled to an array)
static float *tiffu_tget_ll(char *filename_in, int tile_idx,
		int *out_w, int *out_h, int *out_pd)
{

	TIFF *tif = TIFFOpen(filename_in, "r");
	if (!tif) fail("could not open TIFF file \"%s\"", filename_in);

	uint32_t w, h;
	uint16_t spp, bps, fmt;
	int r = 0;
	r += TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	r += TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
	if (r != 2) fail("can not treat TIFF of unkwnown size");
	printf("TIFF %dx%d\n", w, h);

	r = TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
	if(!r) spp=1;
	if(r) printf("TIFF spp %d (r=%d)\n", spp, r);

	r = TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
	if(!r) bps=1;
	if(r) printf("TIFF bps %d (r=%d)\n", spp, r);

	r = TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
	if(!r) fmt = SAMPLEFORMAT_UINT;
	if(r) printf("TIFF fmt %d (r=%d)\n", spp, r);

	if (TIFFIsTiled(tif)) {
		int tisize = TIFFTileSize(tif);
		uint32_t tilewidth, tilelength;
		r = 0;
		r += TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tilewidth);
		r += TIFFGetField(tif, TIFFTAG_TILELENGTH, &tilelength);
		if (r != 2) fail("TIFF tiles of unknown size");

		int ntiles = (w/tilewidth)*(h/tilelength);

		printf("TIFF ntiles %d\n", ntiles);
		printf("TIFF tilesize %d (%dx%d)\n", tisize,
				tilewidth, tilelength);

	}

	TIFFClose(tif);
	return NULL;
}

static void debug_tile_struct(struct tiff_tile *t)
{
	fprintf(stderr, "tile struct at %p\n", (void*)t);
	fprintf(stderr, "\tw = %d\n", t->w);
	fprintf(stderr, "\th = %d\n", t->h);
	fprintf(stderr, "\tspp = %d\n", t->spp);
	fprintf(stderr, "\tbps = %d\n", t->bps);
	fprintf(stderr, "\tfmt = %d\n", t->fmt);
}

// get a tile (high level: output image is saved to a file)
static void tiffu_tget_hl(char *filename_out, char *filename_in, int tile_idx)
{
	struct tiff_tile t[1];
	read_tile_from_file(t, filename_in, tile_idx);

	//debug_tile_struct(t);

	write_tile_to_file(filename_out, t);
	free(t->data);
}

static void tiffu_tput_hl(char *fname_whole, int tile_idx, char *fname_part)
{
	struct tiff_tile t[1];
	read_tile_from_file(t, fname_part, 0);

	put_tile_into_file(fname_whole, t, tile_idx);
}

//void tcrop_old(char *fname_out, char *fname_in, int x0, int xf, int y0, int yf)
//{
//	// open input file
//	TIFF *tif = TIFFOpen(fname_in, "r");
//	if (!tif) fail("can not open TIFF file \"%s\" for reading", fname_in);
//
//	// read input file info
//	struct tiff_info tinfo[1];
//	get_tiff_info(tinfo, tif);
//
//	// adjust crop limits so that they fit inside the input image
//	if (x0 <  0       ) x0 = 0;
//	if (y0 <  0       ) y0 = 0;
//	if (xf >= tinfo->w) xf = tinfo->w - 1;
//	if (yf >= tinfo->h) yf = tinfo->h - 1;
//
//	// create output structure
//	struct tiff_tile tout[1];
//	tout->w = 1 + xf - x0;
//	tout->h = 1 + yf - y0;
//	tout->spp = tinfo->spp;
//	tout->bps = tinfo->bps;
//	tout->fmt = tinfo->fmt;
//	int pixel_size = tinfo->spp * tinfo->bps/8;
//	int output_size = tout->w * tout->h * pixel_size;
//	tout->data = xmalloc(output_size);
//	tout->broken = false;
//
//	// convenience simplifcations (will be removed when the need arises)
//	if (!tinfo->tiled) fail("can only crop tiled files");
//	if (tinfo->packed) fail("can not crop packed images");
//	if (tinfo->broken) fail("can not crop images with broken pixels");
//
//	// compute coordinates of required tiles
//	int tix0 = x0 / tinfo->tw;
//	int tixf = xf / tinfo->tw;
//	int tiy0 = y0 / tinfo->th;
//	int tiyf = yf / tinfo->th;
//
//	// read input tiles
//	int tisize = TIFFTileSize(tif);
//	uint8_t *buf = xmalloc(tisize);
//
//	// the outer 2 loops traverse the needed tiles of the big
//	// the inner 3 loops traverse the needed pixels of each tile
//	// i=tile indexes, ii=tile-wise pixel coords, iii=crop-wise pixel coords
//	for (int j = tiy0; j <= tiyf; j++)
//	for (int i = tix0; i <= tixf; i++)
//	{
//		TIFFReadTile(tif, buf, i*tinfo->tw, j*tinfo->th, 0, 0);
//		int i_0 = i > tix0 ? 0 : x0 % tinfo->tw; // first pixel in tile
//		int j_0 = j > tiy0 ? 0 : y0 % tinfo->th;
//		int i_f = i < tixf ? tinfo->tw-1 : xf % tinfo->tw; // last pixel
//		int j_f = j < tiyf ? tinfo->th-1 : yf % tinfo->th;
//		for (int jj = j_0; jj <= j_f; jj++)
//		for (int ii = i_0; ii <= i_f; ii++)
//		for (int l = 0; l < pixel_size; l++)
//		{
//			int iii = ii + i*tinfo->tw - x0;
//			int jjj = jj + j*tinfo->th - y0;
//			int oidx = l + pixel_size*(iii + jjj*tout->w);
//			int iidx = l + pixel_size*(ii + jj*tinfo->tw);
//			tout->data[oidx] = buf[iidx];
//		}
//	}
//	free(buf);
//
//	// close input file
//	TIFFClose(tif);
//
//	// write output data
//	write_tile_to_file(fname_out, tout);
//	
//	// cleanup
//	free(tout->data);
//}

static void crop_tiles(struct tiff_tile *tout, struct tiff_info *tinfo,
		TIFF *tif, int x0, int xf, int y0, int yf)
{
	// define useful constants
	int pixel_size = tinfo->spp * tinfo->bps/8;
	int output_size = tout->w * tout->h * pixel_size;

	// compute coordinates of the required tiles
	int tix0 = x0 / tinfo->tw;
	int tixf = xf / tinfo->tw;
	int tiy0 = y0 / tinfo->th;
	int tiyf = yf / tinfo->th;
	assert(tix0 <= tixf); assert(tixf < tinfo->ta);
	assert(tiy0 <= tiyf); assert(tiyf < tinfo->td);

	// allocate space for a temporal buffer
	int tisize = TIFFTileSize(tif);
	assert(tisize == tinfo->tw * tinfo->th * tinfo->spp * tinfo->bps/8);
	uint8_t *buf = xmalloc(tisize);

	// the outer 2 loops traverse the needed tiles of the input file
	// the inner 3 loops traverse the needed samples of each tile
	// i=tile indexes, ii=tile-wise pixel coords, iii=crop-wise pixel coords
	for (int j = tiy0; j <= tiyf; j++)
	for (int i = tix0; i <= tixf; i++)
	{
		int r = my_readtile(tif, buf, i*tinfo->tw, j*tinfo->th, 0, 0);
		if (r < 0) fail("could not read tile (%d,%d)");
		int i_0 = i > tix0 ? 0 : x0 % tinfo->tw; // first pixel in tile
		int j_0 = j > tiy0 ? 0 : y0 % tinfo->th;
		int i_f = i < tixf ? tinfo->tw-1 : xf % tinfo->tw; // last pixel
		int j_f = j < tiyf ? tinfo->th-1 : yf % tinfo->th;
		assert(0 <= i_0); assert(i_0 < tinfo->tw);
		assert(0 <= j_0); assert(j_0 < tinfo->th);
		assert(0 <= i_f); assert(i_f < tinfo->tw);
		assert(0 <= j_f); assert(j_f < tinfo->th);
		for (int jj = j_0; jj <= j_f; jj++)
		for (int ii = i_0; ii <= i_f; ii++)
		for (int l = 0; l < pixel_size; l++)
		{
			int iii = ii + i*tinfo->tw - x0;
			int jjj = jj + j*tinfo->th - y0;
			int oidx = l + pixel_size*(iii + jjj*tout->w);
			int iidx = l + pixel_size*(ii + jj*tinfo->tw);
			assert(iii >= 0); assert(iii < tout->w);
			assert(jjj >= 0); assert(jjj < tout->h);
			assert(0 <= oidx); assert(oidx < output_size);
			assert(0 <= iidx); assert(iidx < tisize);
			tout->data[oidx] = buf[iidx];
		}
	}
	free(buf);
}

static void crop_scanlines(struct tiff_tile *tout, struct tiff_info *tinfo,
		TIFF *tif, int x0, int xf, int y0, int yf)
{
	// define useful constants
	int pixel_size = tinfo->spp * tinfo->bps/8;
	int output_size = tout->w * tout->h * pixel_size;

	// allocate space for a temporal buffer
	int scanline_size = TIFFScanlineSize(tif);
	assert(scanline_size == tinfo->w * pixel_size);
	uint8_t *buf = xmalloc(scanline_size);

	// the outer loop traverses the required scanlines of the input file
	// the inner 2 loops traverse the needed samples of each scanline
	for (int j = tinfo->compressed ? 0 : y0; j <= yf; j++)
	{
		int r = TIFFReadScanline(tif, buf, j, 0);
		if (r < 0) fail("could not read scanline %d", j);
		if (j < y0) continue;
		for (int i = x0; i <= xf; i++)
		for (int l = 0; l < pixel_size; l++)
		{
			int ii = i - x0;
			int jj = j - y0;
			int oidx = l + pixel_size*(ii + jj*tout->w);
			int iidx = l + pixel_size*i;
			assert(ii >= 0); assert(ii < tout->w);
			assert(jj >= 0); assert(jj < tout->h);
			assert(0 <= oidx); assert(oidx < output_size);
			assert(0 <= iidx); assert(iidx < scanline_size);
			tout->data[oidx] = buf[iidx];
		}
	}
	free(buf);
}

void tcrop(char *fname_out, char *fname_in, int x0, int xf, int y0, int yf)
{
	// open input file
	TIFF *tif = TIFFOpen(fname_in, "r");
	if (!tif) fail("can not open TIFF file \"%s\" for reading", fname_in);

	// read input file info
	struct tiff_info tinfo[1];
	get_tiff_info(tinfo, tif);

	// adjust crop limits so that they fit inside the input image
	if (x0 <  0       ) x0 = 0;
	if (y0 <  0       ) y0 = 0;
	if (xf >= tinfo->w) xf = tinfo->w - 1;
	if (yf >= tinfo->h) yf = tinfo->h - 1;

	// create output structure
	struct tiff_tile tout[1];
	tout->w = 1 + xf - x0;
	tout->h = 1 + yf - y0;
	tout->spp = tinfo->spp;
	tout->bps = tinfo->bps;
	tout->fmt = tinfo->fmt;
	int pixel_size = tinfo->spp * tinfo->bps/8;
	int output_size = tout->w * tout->h * pixel_size;
	tout->data = xmalloc(output_size);
	tout->broken = false;

	// convenience simplifcations (will be removed when the need arises)
	if (tinfo->packed) fail("can not crop packed images");
	if (tinfo->broken) fail("can not crop images with broken pixels");
	//if (!tinfo->tiled) fail("can only crop tiled files");

	if (tinfo->tiled)
		crop_tiles(tout, tinfo, tif, x0, xf, y0, yf);
	else
		crop_scanlines(tout, tinfo, tif, x0, xf, y0, yf);

	// close input file
	TIFFClose(tif);

	// write output data
	write_tile_to_file(fname_out, tout);

	// cleanup
	free(tout->data);
}

static int main_info(int c, char *v[])
{
	if (c != 2) {
		fprintf(stderr, "usage:\n\t%s file.tiff\n", *v);
		return 1;
	}
	char *filename = v[1];

	struct tiff_info t[1];
	get_tiff_info_filename(t, filename);

	printf("TIFF %d x %d\n", t->w, t->h);
	printf("TIFF spp = %d\n", t->spp);
	printf("TIFF bps = %d\n", t->bps);
	printf("TIFF fmt = \"%s\"", fmt_to_string(t->fmt));
	if (t->tiled) printf(", tiled");
	if (t->broken) printf(", broken");
	if (t->compressed) printf(", compressed");
	if (t->packed) printf(", packed");
	printf("\n");
	//printf("TIFF tiled = %s\n", t->tiled ? "yes" : "no");
	//printf("TIFF broken = %s\n", t->broken ? "yes" : "no");
	//printf("TIFF compressed = %s\n", t->compressed ? "yes" : "no");
	//printf("TIFF packed = %s\n", t->packed ? "yes" : "no");
	if (t->tiled) {
		printf("TIFF ntiles = %d (%d)\n", t->ta * t->td, t->ntiles);
		printf("TIFF tsize = %d (%d x %d)\n",t->tw*t->th, t->tw, t->th);
		printf("TIFF tconfig %d x %d (%d)\n",t->ta, t->td, t->ta*t->td);
	}

	return 0;
}

static void tiffu_print_info2(char *filename)
{
	struct tiff_info t[1];

}

static int main_info2(int c, char *v[])
{
	if (c != 2) {
		fprintf(stderr, "usage:\n\t%s file.tiff\n", *v);
		return 1;
	}
	char *filename = v[1];

	tiffu_print_info2(filename);

	return 0;
}

static int main_whatever(int c, char *v[])
{
	if (c != 2) {
		fprintf(stderr, "usage:\n\t%s file.tiff\n", *v);
		return 1;
	}
	char *filename = v[1];

	tiffu_whatever(filename);

	return 0;
}

static int main_ntiles(int c, char *v[])
{
	if (c != 2) {
		fprintf(stderr, "usage:\n\t%s file.tiff\n", *v);
		return 1;
	}

	//printf("%d (%d)\n", tiffu_ntiles(v[1]), tiffu_ntiles2(v[1]));
	//printf("%d (%d)\n", tiffu_ntiles(v[1]), tiffu_ntiles2(v[1]));
	printf("%d\n", tiffu_ntiles2(v[1]));

	return 0;
}

static int main_tget(int c, char *v[])
{
	if (c != 4) {
		fprintf(stderr, "usage:\n\t%s file.tiff idx til.tiff\n", *v);
		//                          0 1         2   3
		return 1;
	}
	char *filename_in = v[1];
	int tile_idx = atoi(v[2]);
	char *filename_out = v[3];

	tiffu_tget_hl(filename_out, filename_in, tile_idx);

	return 0;
}

static int main_tput(int c, char *v[])
{
	if (c != 4) {
		fprintf(stderr, "usage:\n\t"
				"%s out_whole.tiff idx in_part.tiff\n", *v);
		//                0 1              2   3
		return 1;
	}
	char *filename_whole = v[1];
	int tile_idx = atoi(v[2]);
	char *filename_part = v[3];

	tiffu_tput_hl(filename_whole, tile_idx, filename_part);

	return 0;
}

static void create_zero_tiff_file(char *filename, int w, int h,
		int tw, int th, int spp, int bps, char *fmt, bool incomplete)
{
	//if (bps != 8 && bps != 16 && bps == 32 && bps != 64
	//		&& bps != 128 && bps != 92)
	//	fail("bad bps=%d", bps);
	//if (spp < 1) fail("bad spp=%d", spp);
	int fmt_id = fmt_from_string(fmt);
	double gigabytes = (spp/8.0) * w * h * bps / 1024.0 / 1024.0 / 1024.0;
	fprintf(stderr, "gigabytes = %g\n", gigabytes);
	TIFF *tif = TIFFOpen(filename, gigabytes > 1 ? "w8" : "w");
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
	TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, fmt_id);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_TILEWIDTH, tw);
	TIFFSetField(tif, TIFFTAG_TILELENGTH, th);


	int tilesize = tw * th * bps/8 * spp;
	uint8_t *buf = xmalloc(tilesize);
	for (int i = 0; i < tilesize; i++)
	{
		float x = ((i/((spp*bps)/8))%tw)/((double)tw)-0.5;
		float y = ((i/((spp*bps)/8))/th)/((double)th)-0.5;
		buf[i] = 128+128*sin(90*pow(hypot(x,y+0.3*x),1+(i%spp-1)/1.3));
	}
	TIFFWriteTile(tif, buf, 0, 0, 0, 0);
	TIFFClose(tif);
	if (!incomplete) {
		for (int j = 0; j < h; j += th)
		for (int i = 0; i < w; i += tw)
		{
			tif = TIFFOpen(filename, "r+");
			TIFFWriteTile(tif, buf, i, j, 0, 0);
			TIFFClose(tif);
		}
	}
	free(buf);
}

static void create_zero_tiff_file_tinfo(char *filename,
	       struct tiff_info *t, bool incomplete)
{
	create_zero_tiff_file(filename, t->w, t->h, t->tw, t->th,
			t->spp, t->bps, fmt_to_string(t->fmt), incomplete);
}

#include "pickopt.c"

static int main_tzero(int c, char *v[])
{
	bool i = pick_option(&c, &v, "i", NULL);
	if (c != 9)  {
		fprintf(stderr, "usage:\n\t"
		"%s [-i] w h tw th spp bps {int|uint|ieeefp} out.tiff\n", *v);
	//        0      1 2 3  4  5   6   7   8
		return 0;
	}
	int w = atoi(v[1]);
	int h = atoi(v[2]);
	int tw = atoi(v[3]);
	int th = atoi(v[4]);
	int spp = atoi(v[5]);
	int bps = atoi(v[6]);
	char *fmt = v[7];
	char *filename = v[8];

	create_zero_tiff_file(filename, w, h, tw, th, spp, bps, fmt, i);

	return 0;
}


// metatiler {{{1

#define CMDLINE_MAX 10000
#define MARKER_INPUT  '^'
#define MARKER_OUTPUT '@'

static int system_v(const char *fmt, ...)

{
	char buf[CMDLINE_MAX];
	va_list l;
	va_start(l, fmt);
	vsnprintf(buf, CMDLINE_MAX, fmt, l);
	va_end(l);
	//fprintf(stderr, "SYSTEM_V: %s\n", buf);
	int r = system(buf);
	if (r) fail("SYSTEM(\"%s\") failed\n", buf);
	return 0;
}


static void add_item_to_cmdline(char *cmdline, char *item, char *fileprefix)
{
	//fprintf(stderr, "ADD \"%s%s\"\n", fileprefix?fileprefix:"", item);
	if (*cmdline)
		strncat(cmdline, " ", CMDLINE_MAX);
	if (fileprefix)
		strncat(cmdline, fileprefix, CMDLINE_MAX);
	strncat(cmdline, item, CMDLINE_MAX);
}

static char *create_temporary_directory(void)
{
	//return "/tmp/metafilter_temporary_directory/";
	return "/tmp/MTD/";
}

static char *bn(char *s)
{
	char *r = strrchr(s, '/');
	r = r ? r + 1 : s;
	return r;
}

static char *pn(char *p, char *s)
{
	static char r[CMDLINE_MAX];
	snprintf(r, CMDLINE_MAX, "%s%s", p, bn(s));
	return r;
}

static void fill_subs_cmdline(char *cmdline, char *cmd, char *fprefix,
		char **fns_in, int n_in, char **fns_out, int n_out)
{
	*cmdline = 0;
	char *tok = strtok(cmd, " ");
	do {
		if (*tok=='>' || *tok=='|' || *tok=='<') {
			fprintf(stderr, "ERROR: must be a single "
					"command line\n");
			exit(1);
		} else if (*tok == MARKER_INPUT) {
			int idx = atoi(tok+1)-1;
			if (idx >= 0 && idx < n_in)
			add_item_to_cmdline(cmdline, bn(fns_in[idx]), fprefix);
		} else if (*tok == MARKER_OUTPUT) {
			int idx = atoi(tok+1)-1;
			if (idx >= 0 && idx < n_out)
			add_item_to_cmdline(cmdline, bn(fns_out[idx]), fprefix);
		} else
			add_item_to_cmdline(cmdline, tok, NULL);
	} while (tok = strtok(NULL, " "));
}

static void extract_tile(char *tpd, char *filename, int idx)
{
	// 1. open large tiff image "filename"
	// 2. locate idexed tile
	// 3. read indexed tile data
	// 4. close large tiff image
	// 5. create new small tiff image "tpd/filename"
	// 6. write data to new small tiff image
}

static void paste_tile(char *filename, int idx, char *tpd)
{
	// 1. read data from small tiff file
	// 2. open large tiff file
	// 3. locate position of indexed tile
	// 4. rewrite indexed tile with new data
	// 5. save and close the large tiff file
}

void metatiler(char *command, char **fname_in, int n_in,
		char **fname_out, int n_out)
{
	// build command line (will be the same at each run)
	char cmdline[CMDLINE_MAX];
	char *tpd = create_temporary_directory();
	fill_subs_cmdline(cmdline, command, tpd,
			fname_in, n_in, fname_out, n_out);
	fprintf(stderr, "CMDLINE = \"%s\"\n", cmdline);

	// create temporary filenames
	char tname_in[n_in][FILENAME_MAX];
	char tname_out[n_out][FILENAME_MAX];
	for (int i = 0; i < n_in; i++)
		snprintf(tname_in[i],FILENAME_MAX,"%s%s",tpd,bn(fname_in[i]));
	for (int i = 0; i < n_out; i++)
		snprintf(tname_out[i],FILENAME_MAX,"%s%s",tpd,bn(fname_out[i]));

	// determine input tile geometry
	struct tiff_info tinfo_in[n_in], tinfo_out[n_out];
	for (int i = 0; i < n_in; i++)
		get_tiff_info_filename(tinfo_in + i, fname_in[i]);

	// check tile geometry consistency
	for (int i = 1; i < n_in; i++)
	{
		struct tiff_info *ta = tinfo_in + 0;
		struct tiff_info *tb = tinfo_in + i;
		if (ta->w != tb->w || ta->h != ta->h)
			fail("image \"%s\" size mismatch (%dx%d != %dx%d)\n",
				fname_in[i], ta->w, ta->h, tb->w, tb->h);
		if (ta->ntiles != tb->ntiles)
			fail("image \"%s\" ntiles mismatch (%d != %d)\n",
				fname_in[i], ta->ntiles, tb->ntiles);
	}

	// do it!
	for (int i = 0; i < tinfo_in->ntiles; i++)
	{
		fprintf(stderr, "process tile %d / %d\n", i+1,tinfo_in->ntiles);
		for (int k = 0; k < n_in; k++)
			system_v("tiffu tget %s %d %s",
				fname_in[k], i, tname_in[k]);
		system_v("%s", cmdline);
		if (!i) for (int k = 0; k < n_out; k++)
		{
			struct tiff_info *t = tinfo_out + k;
			get_tiff_info_filename(t, tname_out[k]);
			t->w = tinfo_in->w;
			t->h = tinfo_in->h;
			t->tw = tinfo_in->tw;
			t->th = tinfo_in->th;
			create_zero_tiff_file_tinfo(fname_out[k], t, true);
		}
		for (int k = 0; k < n_out; k++)
			system_v("tiffu tput %s %d %s",
				fname_out[k], i, tname_out[k]);
	}

	//for (int i = 0; i < ntiles; i++)
	//{
	//	// 1. extract ith tile from input images
	//	// 2. run cmdline
	//	fprintf(stderr, "run \"%s\"\n", cmdline);
	//	// 3. paste results to output files
	//}
}

int main_meta(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "usage:\n\t"
			"%s \"CMD <1 <2 >1\" in1 in2 -- out1\n", *argv);
		//       0   1               2   3   ...
		return 1;
	}

	// get input arguments
	char *command = argv[1];
	int n_in = 0, n_out = 0;
	char *filenames_in[argc];
	char *filenames_out[argc];
	for (int i = 2; i < argc && strcmp(argv[i], "--"); i++)
		filenames_in[n_in++] = argv[i];
	for (int i = 3+n_in; i < argc; i++)
		filenames_out[n_out++] = argv[i];

	// print debug info
	fprintf(stderr, "%d input files:\n", n_in);
	for (int i = 0; i < n_in; i++)
		fprintf(stderr, "\t%s\n", filenames_in[i]);
	fprintf(stderr, "%d output files:\n", n_out);
	for (int i = 0; i < n_out; i++)
		fprintf(stderr, "\t%s\n", filenames_out[i]);
	fprintf(stderr, "COMMAND = \"%s\"\n", command);

	// run program
	metatiler(command, filenames_in, n_in, filenames_out, n_out);

	// exit
	return 0;
}

void metatiler_low_level(char *command, char **filenames_in, int n_in,
		char **filenames_out, int n_out)
{
	// build command line (will be the same at each run)
	char cmdline[CMDLINE_MAX];
	char *tpd = create_temporary_directory();
	fill_subs_cmdline(cmdline, command, tpd,
			filenames_in, n_in, filenames_out, n_out);

	// determine tile geometry
	int ntiles = 4;//=get_number_of_tiles(*filename_in)

	// create output files as empty files with the required tile geometry
	// to solve: what pixel dimension and type?
	// solution: run the program with the first tile and see what happens

	for (int i = 0; i < ntiles; i++)
	{
		// 1. extract ith tile from input images
		// 2. run cmdline
		fprintf(stderr, "run \"%s\"\n", cmdline);
		// 3. paste results to output files
	}
}

static void metatiler_bad(int argc, char *argv[],
		int *idx_in, int n_in, int *idx_out, int n_out)
{
	// 1. build generic command line
	//char command[CMDLINE_MAX]; ncommand = 0;
	int patn = 0;
	char pat[argc][3];
	char *pat_argv[argc];

	for (int i = 0; i < argc; i++)
	{
		pat_argv[i] = argv[i];
		fprintf(stderr, "pat_argv[%d] = \"%s\"\n", i, pat_argv[i]);
	}

	fprintf(stderr, "COMMAND = %s\n", argv[1]);
	for (int i = 0; i < n_in; i++)
	{
		fprintf(stderr, "IN_%d = %s\n", 1+i, argv[idx_in[i]]);
		pat[patn][0] = '<';
		pat[patn][1] = '1' + i;
		pat[patn][2] = '\0';
		pat_argv[idx_in[i]] = pat[patn];
		patn += 1;
	}
	for (int i = 0; i < n_out; i++)
	{
		fprintf(stderr, "OUT_%d = %s\n", 1+i, argv[idx_out[i]]);
		pat[patn][0] = '>';
		pat[patn][1] = '1' + i;
		pat[patn][2] = '\0';
		pat_argv[idx_out[i]] = pat[patn];
		patn += 1;
	}

	for (int i = 0; i < argc; i++)
		fprintf(stderr, "pat_argv[%d] = \"%s\"\n", i, pat_argv[i]);


	// 2. assess input files consistency

	// 3. create output file
	
	// 4. create temporary directory
	
	// 5. execute command for each tile, copying back the tile to 
}

static bool hassuffix(const char *s, const char *suf)
{
	int len_s = strlen(s);
	int len_suf = strlen(suf);
	if (len_s < len_suf)
		return false;
	return 0 == strcmp(suf, s + (len_s - len_suf));
}

#define LENGTH(t) sizeof((t))/sizeof((t)[0])

static bool filename_is_tiff(char *s)
{
	char *suf[] = { ".tiff", ".tif", ".TIFF", ".TIF" };
	for (int i = 0; i < LENGTH(suf); i++)
		if (hassuffix(s, suf[i]))
			return true;
	return false;
}

// by default, the last "TIFF" argument is the output, and the rest are
// the input files.  At least one input file is needed, with the desired
// tile configuration.
static int main_meta_old(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "usage:\n\t%s cmd arg1 ... argn\n", *argv);
		//                          0 1   2        c-1
		return 1;
	}
	char *command = argv[1];
	int n_in = 0, n_out = 0;
	int filenames_in[argc];
	int filenames_out[argc];
	for (int i = 2; i < argc; i++)
		if (filename_is_tiff(argv[i]))
			filenames_in[n_in++] = i;
	if (n_in < 2) fail("need at least 2 tiff filenames");
	filenames_out[n_out++] = filenames_in[--n_in];

	//fprintf(stderr, "CMD = %s\n", command);
	//for (int i = 0; i < n_in; i++)
	//	fprintf(stderr, "in_%d = %s\n", 1+i, argv[filenames_in[i]]);
	//for (int i = 0; i < n_out; i++)
	//	fprintf(stderr, "out_%d = %s\n", 1+i, argv[filenames_out[i]]);

	metatiler_bad(argc, argv, filenames_in, n_in, filenames_out, n_out);

	return 0;
}

// getpixel cache {{{1

struct tiff_tile_cache {
	// essential data
	//
	char filename[FILENAME_MAX];
	struct tiff_info i[1];
	void **c;        // pointers to cached tiles

	// data only necessary to delete tiles when the memory is full
	//
	unsigned int *a; // access counter for each tile
	unsigned int ax; // global access counter
	int curtiles;    // current number of tiles in memory
	int maxtiles;    // number of tiles allowed to be in memory at once
};

void tiff_tile_cache_init(struct tiff_tile_cache *t, char *fname, int megabytes)
{
	// set up essential data
	strncpy(t->filename, fname, FILENAME_MAX);
	get_tiff_info_filename(t->i, fname);
	if (t->i->bps < 8 || t->i->packed)
		fail("caching of packed samples is not supported");
	t->c = malloc(t->i->ntiles * sizeof*t->c);
	for (int i = 0; i < t->i->ntiles; i++)
		t->c[i] = 0;

	// set up data for old tile deletion
	if (megabytes) {
		t->a = malloc(t->i->ntiles * sizeof*t->a);
		for (int i = 0; i < t->i->ntiles; i++)
			t->a[i] = 0;
		t->ax = 0;
		int tilesize = t->i->tw * t->i->th * (t->i->bps/8) * t->i->spp;
		double mbts = tilesize / (1024.0 * 1024);
		t->maxtiles = megabytes / mbts;
		t->curtiles = 0;
		fprintf(stderr, "cache: %d tiles (%d megabytes)\n", t->maxtiles, megabytes);
	} else {
		// unlimited tile usage
		t->a = NULL;
	}
}

void tiff_tile_cache_free(struct tiff_tile_cache *t)
{
	for (int i = 0; i < t->i->ntiles; i++)
		free(t->c[i]);
	free(t->c);
	free(t->a);
}

static int my_computetile(struct tiff_info *t, int i, int j)
{
	if (i < 0 || j < 0 || i >= t->w || j >= t->h)
		return -1;
		//fail("got bad pixel (%d %d) [%d %d]", i, j, t->w, t->h);
	int ti = i / t->tw;
	int tj = j / t->th;
	int r = tj * t->ta + ti;
	if (r < 0 || r >= t->ntiles)
		fail("bad tile index %d for point (%d %d)", r, i, j);
	return r;
}

static void notify_tile_access(struct tiff_tile_cache *t, int i)
{
	t->a[i] = ++t->ax;
}

static void free_oldest_tile(struct tiff_tile_cache *t)
{
	// find oldest tile
	int imin = -1;
	for (int i = 0; i < t->i->ntiles; i++)
		if (t->a[i]) {
			if (imin >= 0) {
				if (t->a[i] < t->a[imin])
					imin = i;
			} else {
				imin = i;
			}
		}
	assert(imin >= 0);
	assert(t->a[imin] > 0);

	// free it
	free(t->c[imin]);
	t->c[imin] = 0;
	t->a[imin] = 0;
	t->curtiles -= 1;
	//fprintf(stderr, "left tile %d\n", imin);
}

// assumes pixel coordinates are valid
void *tiff_tile_cache_getpixel(struct tiff_tile_cache *t, int i, int j)
{
	int tidx = my_computetile(t->i, i, j);
	if (tidx < 0) return NULL;
	if (!t->c[tidx]) {
		if (t->a && t->curtiles == t->maxtiles)
			free_oldest_tile(t);

		struct tiff_tile tmp[1];
		//fprintf(stderr, "got: tile %d\n", tidx);
		read_tile_from_file(tmp, t->filename, tidx);
		t->c[tidx] = tmp->data;

		t->curtiles += 1;
	}
	if (t->a) notify_tile_access(t, tidx);

	int ii = i % t->i->tw;
	int jj = j % t->i->th;
	int pixel_index = jj * t->i->tw + ii;
	int pixel_position = pixel_index * t->i->spp * (t->i->bps / 8);
	return pixel_position + (char*)t->c[tidx];
}

static void convert_pixel_to_float(float *out, struct tiff_info *t, void *in)
{
	for (int i = 0; i < t->spp; i++)
	{
		float r;
		switch(t->fmt) {
		case SAMPLEFORMAT_UINT:
			if (t->bps == 8)  r = ((uint8_t*) in)[i];
			if (t->bps == 16) r = ((uint16_t*)in)[i];
			if (t->bps == 32) r = ((uint32_t*)in)[i];
			break;
		case SAMPLEFORMAT_INT:
			if (t->bps == 8)  r = ((int8_t*)  in)[i];
			if (t->bps == 16) r = ((int16_t*) in)[i];
			if (t->bps == 32) r = ((int32_t*) in)[i];
			break;
		case SAMPLEFORMAT_IEEEFP:
			if (t->bps == 32) r = ((float*)   in)[i];
			if (t->bps == 64) r = ((double*)  in)[i];
			break;
		default:
			fail("unrecognized format %d", t->fmt);
		}
		out[i] = r;
	}
}

static int main_getpixel(int c, char *v[])
{
	char *oM = pick_option(&c, &v, "m", "0");
	if (c != 2) {
		fprintf(stderr, "usage:\n\techo i j | %s file.tiff\n", *v);
		return 1;
	}
	char *filename_in = v[1];
	int megabytes = atoi(oM);

	struct tiff_tile_cache t[1];
	tiff_tile_cache_init(t, filename_in, megabytes);

	int i, j;
	while (2 == scanf("%d %d\n", &i, &j))
	{
		// read pixel
		void *p = tiff_tile_cache_getpixel(t, i, j);

		// convert to float
		float x[t->i->spp];
		convert_pixel_to_float(x, t->i, p);

		// print samples to stdout
		printf("%d\t%d", i, j);
		for (int i = 0; i < t->i->spp; i++)
			printf("\t%g", x[i]);
		printf("\n");
	}

	tiff_tile_cache_free(t);
	return 0;
}

static void zoom_out_by_factor_two(char *fname_out, char *fname_in, int type)
{
	// get information of input file
	struct tiff_info tin[1], tout[1];
	get_tiff_info_filename(tin, fname_in);
	if (!tin->tiled) fail("I can only zoom out tiled images");

	// create output file
	tout[0] = tin[0];
	tout->w = ceil(tin->w/2.0);
	tout->h = ceil(tin->h/2.0);
	create_zero_tiff_file_tinfo(fname_out, tout, true);
	get_tiff_info_filename(tout, fname_out);

	// create buffer tile for output file
	struct tiff_tile buf[1];
	read_tile_from_file(buf, fname_out, 0);

	// create cache structure for input file
	int tilesize = tinfo_tilesize(tin);
	int megabytes = tilesize / 250000;
	if (megabytes < 100) megabytes = 100;
	struct tiff_tile_cache cin[1];
	tiff_tile_cache_init(cin, fname_in, megabytes);

	// fill-in tiles
	for (int tj = 0; tj < tout->td; tj++)
	for (int ti = 0; ti < tout->ta; ti++)
	{
		int offx = ti * tout->tw;
		int offy = tj * tout->th;
		int tidx = my_computetile(tout, offx, offy);
		memset(buf->data, tidx, tilesize);
		if (tidx < 0) fail("offsets %d %d outside", offx, offy);
		for (int j = 0; j < tout->th; j++)
		for (int i = 0; i < tout->tw; i++)
		{
			int neig[4][2] = { {0,0}, {1,0}, {0,1}, {1,1}};
			void *p[4];
			for (int k = 0; k < 4; k++)
			{
				int ii = 2 * (offx + i) + neig[k][0];
				int jj = 2 * (offy + j) + neig[k][1];
				p[k] = tiff_tile_cache_getpixel(cin, ii, jj);
			}
			if (!p[0]) continue;
			int psiz = tinfo_pixelsize(tout);
			int ppos = (i + j * buf->w) * psiz;
			void *pdest = ppos + (char*)buf->data;
			memcpy(pdest, p[0], psiz);
		}
		put_tile_into_file(fname_out, buf, tidx);
	}
	free(buf->data);
	tiff_tile_cache_free(cin);
}

static int main_zoomout(int c, char *v[])
{
	if (c != 4) {
		fprintf(stderr, "usage:\n\t"
				"%s {f|v|i|a} in.tiff out.tiff\n", *v);
		//                0  1        2       3
		return 1;
	}
	int type = v[1][0];
	char *filename_in = v[2];
	char *filename_out = v[3];

	zoom_out_by_factor_two(filename_out, filename_in, type);

	return 0;
}

static int main_crop(int c, char *v[])
{
	if (c != 6) {
		fprintf(stderr, "usage:\n\t%s cx cy r in.tiff out.tiff\n", *v);
		//                          0 1  2  3 4       5
		return 1;
	}
	int cx = atoi(v[1]);
	int cy = atoi(v[2]);
	int rad = atoi(v[3]);
	char *filename_in = v[4];
	char *filename_out = v[5];

	int xmin = cx - rad;
	int ymin = cy - rad;
	int xmax = cx + rad;
	int ymax = cy + rad;

	tcrop(filename_out, filename_in, xmin, xmax, ymin, ymax);

	return 0;
}

static void my_putchar(FILE *f, int c)
{
	fputc(c, f);
}

static int main_imprintf(int argc, char *argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage:\n\t%s format image\n", *argv);
		//                          0 1      2
		return 1;
	}
	char *format = argv[1];
	char *filename = argv[2];

	struct tiff_info t;
	get_tiff_info_filename(&t, filename);

	FILE *f = stdout;
	while (*format) {
		int c = *format++;
		if (c == '%') {
			c = *format++; if (!c) break;
			int p = -1;
			switch(c) {
			case 'w': p = t.w; break;
			case 'h': p = t.h; break;
			case 'n': p = t.w * t.h; break;
			case 'd': p = t.spp; break;
			case 'b': p = t.bps; break;
			case 'B': p = t.bps/8; break;
			case 'p': p = t.spp * t.bps; break;
			case 'P': p = t.spp * t.bps / 8; break;
			case 'f': p = t.fmt == SAMPLEFORMAT_IEEEFP; break;
			case 'u': p = t.fmt == SAMPLEFORMAT_UINT; break;
			case 'i': p = t.fmt == SAMPLEFORMAT_INT; break;
			case 'T': p = t.tiled; break;
			case 'Z': p = t.broken; break;
			case 'W': p = !t.tiled ? 0 : t.tw; break;
			case 'H': p = !t.tiled ? 0 : t.th; break;
			case 'A': p = !t.tiled ? 0 : t.ta; break;
			case 'D': p = !t.tiled ? 0 : t.td; break;
			case 'N': p = !t.tiled ? 0 : t.ta * t.td; break;
			default: break;
			}
			fprintf(f, "%d", p);
		} else if (c == '\\') {
			c = *format++; if (!c) break;
			if (c == '%') my_putchar(f, '%');
			if (c == 'n') my_putchar(f, '\n');
			if (c == 't') my_putchar(f, '\t');
			if (c == '"') my_putchar(f, c);
			if (c == '\'') my_putchar(f, c);
			if (c == '\\') my_putchar(f, c);
		} else
			my_putchar(f, c);
	}

	return 0;
}

void my_tifferror(const char *module, const char *fmt, va_list ap)
{
	if (0 == strcmp(fmt, "%llu: Invalid tile byte count, tile %lu"))
		fprintf(stderr, "got a zero tile\n");
	else
		fprintf(stderr, "TIFF ERROR(%s): \"%s\"\n", module, fmt);
}

int main(int c, char *v[])
{
	TIFFSetWarningHandler(NULL);//suppress warnings
	//TIFFSetErrorHandler(my_tifferror);

	if (c < 2) {
	err:	fprintf(stderr, "usage:\n\t%s {info|ntiles|tget|tput}\n", *v);
		return 1;
	}

	if (0 == strcmp(v[1], "info"))     return main_info    (c-1, v+1);
	if (0 == strcmp(v[1], "imprintf")) return main_imprintf(c-1, v+1);
	if (0 == strcmp(v[1], "ntiles"))   return main_ntiles  (c-1, v+1);
	if (0 == strcmp(v[1], "tget"))     return main_tget    (c-1, v+1);
	if (0 == strcmp(v[1], "whatever")) return main_whatever(c-1, v+1);
	if (0 == strcmp(v[1], "crop"))     return main_crop    (c-1, v+1);
	if (0 == strcmp(v[1], "tput"))     return main_tput    (c-1, v+1);
	if (0 == strcmp(v[1], "tzero"))    return main_tzero   (c-1, v+1);
	if (0 == strcmp(v[1], "meta"))     return main_meta    (c-1, v+1);
	if (0 == strcmp(v[1], "getpixel")) return main_getpixel(c-1, v+1);
	if (0 == strcmp(v[1], "zoomout"))  return main_zoomout (c-1, v+1);

	goto err;
}
