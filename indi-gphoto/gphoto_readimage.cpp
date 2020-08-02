
#include "gphoto_readimage.h"

#include <indilogger.h>

#include <jpeglib.h>
#include <fitsio.h>
#include <libraw.h>

#include <unistd.h>
#include <arpa/inet.h>


char device[64];

#define err_printf IDLog,

void gphoto_read_set_debug(const char *name)
{
    strncpy(device, name, 64);
}

void addFITSKeywords(fitsfile *fptr)
{
    int status = 0;

    /* TODO add other data later */
    fits_write_date(fptr, &status);
}

int read_libraw(const char *filename, uint8_t **memptr, size_t *memsize, int *n_axis, int *w, int *h, int *bitsperpixel,
                char *bayer_pattern)
{
    int ret = 0;
    // Creation of image processing object
    LibRaw RawProcessor;

    // Let us open the file
    if ((ret = RawProcessor.open_file(filename)) != LIBRAW_SUCCESS)
    {
        DEBUGFDEVICE(device, INDI::Logger::DBG_ERROR, "Cannot open %s: %s", filename, libraw_strerror(ret));
        RawProcessor.recycle();
        return -1;
    }

    // Let us unpack the image
    if ((ret = RawProcessor.unpack()) != LIBRAW_SUCCESS)
    {
        DEBUGFDEVICE(device, INDI::Logger::DBG_ERROR, "Cannot unpack %s: %s", filename, libraw_strerror(ret));
        RawProcessor.recycle();
        return -1;
    }

    *n_axis       = 2;
    *w            = RawProcessor.imgdata.rawdata.sizes.raw_width;
    *h            = RawProcessor.imgdata.rawdata.sizes.raw_height;
    *bitsperpixel = 16;
    // cdesc contains counter-clock wise e.g. RGBG CFA pattern while we want it sequential as RGGB
    // coordinats are relative to top_margin and left_margin
    // since libraw-0.20 the bayer pattern of the black frame is identical
    bayer_pattern[0] = RawProcessor.imgdata.idata.cdesc[RawProcessor.COLOR(0, 0)];
    bayer_pattern[1] = RawProcessor.imgdata.idata.cdesc[RawProcessor.COLOR(0, 1)];
    bayer_pattern[2] = RawProcessor.imgdata.idata.cdesc[RawProcessor.COLOR(1, 0)];
    bayer_pattern[3] = RawProcessor.imgdata.idata.cdesc[RawProcessor.COLOR(1, 1)];
    bayer_pattern[4] = '\0';

    DEBUGFDEVICE(device, INDI::Logger::DBG_DEBUG,
                 "read_libraw: raw_width: %d top_margin %d left_margin %d",
                 RawProcessor.imgdata.rawdata.sizes.raw_width, RawProcessor.imgdata.sizes.top_margin,
                 RawProcessor.imgdata.sizes.left_margin);

    *memsize = RawProcessor.imgdata.rawdata.sizes.raw_width * RawProcessor.imgdata.rawdata.sizes.raw_height * sizeof(uint16_t);
    *memptr  = (uint8_t *)realloc(*memptr, *memsize);

    DEBUGFDEVICE(device, INDI::Logger::DBG_DEBUG,
                 "read_libraw: rawdata.sizes.width: %d rawdata.sizes.height %d memsize %d bayer_pattern %s",
                 RawProcessor.imgdata.rawdata.sizes.width, RawProcessor.imgdata.rawdata.sizes.height, *memsize,
                 bayer_pattern);

    uint16_t *image = reinterpret_cast<uint16_t *>(*memptr);
    uint16_t *src   = RawProcessor.imgdata.rawdata.raw_image;

    memcpy(image, src, RawProcessor.imgdata.rawdata.sizes.raw_height * RawProcessor.imgdata.rawdata.sizes.raw_width * 2);

    return 0;
}

int read_jpeg(const char *filename, uint8_t **memptr, size_t *memsize, int *naxis, int *w, int *h)
{
    unsigned char *r_data = nullptr, *g_data = nullptr, *b_data = nullptr;

    /* these are standard libjpeg structures for reading(decompression) */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    /* libjpeg data structure for storing one row, that is, scanline of an image */
    JSAMPROW row_pointer[1] = { nullptr };

    FILE *infile = fopen(filename, "rb");

    if (!infile)
    {
        DEBUGFDEVICE(device, INDI::Logger::DBG_DEBUG, "Error opening jpeg file %s!", filename);
        return -1;
    }
    /* here we set up the standard libjpeg error handler */
    cinfo.err = jpeg_std_error(&jerr);
    /* setup decompression process and source, then read JPEG header */
    jpeg_create_decompress(&cinfo);
    /* this makes the library read from infile */
    jpeg_stdio_src(&cinfo, infile);
    /* reading the image header which contains image information */
    jpeg_read_header(&cinfo, (boolean)TRUE);

    /* Start decompression jpeg here */
    jpeg_start_decompress(&cinfo);

    *memsize = cinfo.output_width * cinfo.output_height * cinfo.num_components;
    *memptr  = (uint8_t *)realloc(*memptr, *memsize);
    uint8_t *oldmem =
        *memptr; // if you do some ugly pointer math, remember to restore the original pointer or some random crashes will happen. This is why I do not like pointers!!
    *naxis = cinfo.num_components;
    *w     = cinfo.output_width;
    *h     = cinfo.output_height;

    /* now actually read the jpeg into the raw buffer */
    row_pointer[0] = (unsigned char *)malloc(cinfo.output_width * cinfo.num_components);
    if (cinfo.num_components)
    {
        r_data = (unsigned char *)*memptr;
        g_data = r_data + cinfo.output_width * cinfo.output_height;
        b_data = r_data + 2 * cinfo.output_width * cinfo.output_height;
    }
    /* read one scan line at a time */
    for (unsigned int row = 0; row < cinfo.image_height; row++)
    {
        unsigned char *ppm8 = row_pointer[0];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);

        if (cinfo.num_components == 3)
        {
            for (unsigned int i = 0; i < cinfo.output_width; i++)
            {
                *r_data++ = *ppm8++;
                *g_data++ = *ppm8++;
                *b_data++ = *ppm8++;
            }
        }
        else
        {
            memcpy(*memptr, ppm8, cinfo.output_width);
            *memptr += cinfo.output_width;
        }
    }

    /* wrap up decompression, destroy objects, free pointers and close open files */
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if (row_pointer[0])
        free(row_pointer[0]);
    if (infile)
        fclose(infile);

    *memptr = oldmem;

    return 0;
}

int read_jpeg_mem(unsigned char *inBuffer, unsigned long inSize, uint8_t **memptr, size_t *memsize, int *naxis, int *w,
                  int *h)
{
    /* these are standard libjpeg structures for reading(decompression) */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    /* libjpeg data structure for storing one row, that is, scanline of an image */
    JSAMPROW row_pointer[1] = { nullptr };

    /* here we set up the standard libjpeg error handler */
    cinfo.err = jpeg_std_error(&jerr);
    /* setup decompression process and source, then read JPEG header */
    jpeg_create_decompress(&cinfo);
    /* this makes the library read from infile */
    jpeg_mem_src(&cinfo, inBuffer, inSize);

    /* reading the image header which contains image information */
    jpeg_read_header(&cinfo, (boolean)TRUE);

    /* Start decompression jpeg here */
    jpeg_start_decompress(&cinfo);

    *memsize = cinfo.output_width * cinfo.output_height * cinfo.num_components;
    *memptr  = (uint8_t *)realloc(*memptr, *memsize);

    uint8_t *destmem = *memptr;

    *naxis = cinfo.num_components;
    *w     = cinfo.output_width;
    *h     = cinfo.output_height;

    /* now actually read the jpeg into the raw buffer */
    row_pointer[0] = (unsigned char *)malloc(cinfo.output_width * cinfo.num_components);

    /* read one scan line at a time */
    for (unsigned int row = 0; row < cinfo.image_height; row++)
    {
        unsigned char *ppm8 = row_pointer[0];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
        memcpy(destmem, ppm8, cinfo.output_width * cinfo.num_components);
        destmem += cinfo.output_width * cinfo.num_components;
    }

    /* wrap up decompression, destroy objects, free pointers and close open files */
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if (row_pointer[0])
        free(row_pointer[0]);

    return 0;
}

int read_jpeg_size(unsigned char *inBuffer, unsigned long inSize, int *w, int *h)
{
    /* these are standard libjpeg structures for reading(decompression) */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    /* here we set up the standard libjpeg error handler */
    cinfo.err = jpeg_std_error(&jerr);
    /* setup decompression process and source, then read JPEG header */
    jpeg_create_decompress(&cinfo);
    /* this makes the library read from infile */
    jpeg_mem_src(&cinfo, inBuffer, inSize);

    /* reading the image header which contains image information */
    jpeg_read_header(&cinfo, (boolean)TRUE);

    *w     = cinfo.image_width;
    *h     = cinfo.image_height;

    /* wrap up decompression, destroy objects, free pointers and close open files */
    jpeg_destroy_decompress(&cinfo);

    return 0;
}
