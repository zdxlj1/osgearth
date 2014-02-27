/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2013 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/ImageUtils>
#include <osgEarth/ThreadingUtils>
#include <osg/Notify>
#include <osg/Texture>
#include <osg/ImageSequence>
#include <osg/Timer>
#include <osgDB/Registry>
#include <string.h>
#include <memory.h>

#define LC "[ImageUtils] "


#if defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE)
#    define GL_RGB8_INTERNAL  GL_RGB8_OES
#    define GL_RGB8A_INTERNAL GL_RGBA8_OES
#else
#    define GL_RGB8_INTERNAL  GL_RGB8
#    define GL_RGB8A_INTERNAL GL_RGBA8
#endif


using namespace osgEarth;

osg::Image*
ImageUtils::cloneImage( const osg::Image* input )
{
    // Why not just call image->clone()? Because, the osg::Image copy constructor does not
    // clear out the underlying BufferData/BufferObject's GL handles. This can cause 
    // exepected results if you are cloning an image that has already been used in GL.
    // Calling clone->dirty() might work, but we are not sure.

    if ( !input ) return 0L;
    
    osg::Image* clone = osg::clone( input, osg::CopyOp::DEEP_COPY_ALL );
    clone->dirty();
    return clone;
}

void
ImageUtils::normalizeImage( osg::Image* image )
{
    // OpenGL is lax about internal texture formats, and e.g. allows GL_RGBA to be used
    // instead of the proper GL_RGBA8, etc. Correct that here, since some of our compositors
    // rely on having a proper internal texture format.
    if ( image->getDataType() == GL_UNSIGNED_BYTE )
    {
        if ( image->getPixelFormat() == GL_RGB )
            image->setInternalTextureFormat( GL_RGB8_INTERNAL );
        else if ( image->getPixelFormat() == GL_RGBA )
            image->setInternalTextureFormat( GL_RGB8A_INTERNAL );
    }
}

bool
ImageUtils::copyAsSubImage(const osg::Image* src, osg::Image* dst, int dst_start_col, int dst_start_row)
{
    if (!src || !dst ||
        dst_start_col + src->s() > dst->s() ||
        dst_start_row + src->t() > dst->t() ||
        src->r() != dst->r())
    {
        return false;
    }

    // check for fast bytewise copy:
    if (src->getPacking() == dst->getPacking() &&
        src->getDataType() == dst->getDataType() &&
        src->getPixelFormat() == dst->getPixelFormat() &&
        src->getInternalTextureFormat() == dst->getInternalTextureFormat() )
    {
        for(int r=0; r<src->r(); ++r) // each layer
        {
            for( int src_row=0, dst_row=dst_start_row; src_row < src->t(); src_row++, dst_row++ )
            {
                const void* src_data = src->data( 0, src_row, r );
                void* dst_data = dst->data( dst_start_col, dst_row, r );
                memcpy( dst_data, src_data, src->getRowSizeInBytes() );
            }
        }
    }

    // otherwise loop through an convert pixel-by-pixel.
    else
    {
        if ( !PixelReader::supports(src) || !PixelWriter::supports(dst) )
            return false;

        PixelReader read(src);
        PixelWriter write(dst);

        for( int r=0; r<src->r(); ++r)
        {
            for( int src_t=0, dst_t=dst_start_row; src_t < src->t(); src_t++, dst_t++ )
            {
                for( int src_s=0, dst_s=dst_start_col; src_s < src->s(); src_s++, dst_s++ )
                {           
                    write(read(src_s, src_t, r), dst_s, dst_t, r);
                }
            }
        }
    }

    return true;
}  

osg::Image*
ImageUtils::createBumpMap(const osg::Image* input)
{
    if ( !PixelReader::supports(input) || !PixelWriter::supports(input) )
        return 0L;

    osg::Image* output = osg::clone(input, osg::CopyOp::DEEP_COPY_ALL);

    static const float kernel[] = {
        -1.0, -1.0, 0.0,
        -1.0,  0.0, 1.0,
         0.0,  1.0, 1.0 
    };

    PixelReader read(input);
    PixelWriter write(output);

    osg::Vec4f mid(0.5f,0.5f,0.5f,0.5f);

    for( int t=0; t<input->t(); ++t )
    {
        for( int s=0; s<input->s(); ++s )
        {
            if ( t == 0 || t == input->t()-1 || s == 0 || s == input->s()-1 )
            {
                write( mid, s, t );
            }
            else
            {
                osg::Vec4f sum;

                // run the emboss kernel:
                for( int tt=0; tt<=2; ++tt )
                    for( int ss=0; ss<=2; ++ss )
                        sum += read(s+ss-1,t+tt-1) * kernel[tt*3+ss];
                sum /= 9.0f;

                // bias for bumpmapping:
                sum += osg::Vec4f(0.5f,0.5f,0.5f,0.5f);

                // convert to greyscale:
                sum.r() *= 0.2989f;
                sum.g() *= 0.5870f;
                sum.b() *= 0.1140f;

                sum.a() = read(s,t).a();
                write( sum, s, t );
            }
        }
    }
    return output;
}

bool
ImageUtils::resizeImage(const osg::Image* input, 
                        unsigned int out_s, unsigned int out_t, 
                        osg::ref_ptr<osg::Image>& output,
                        unsigned int mipmapLevel,
                        bool bilinear)
{
    if ( !input && out_s == 0 && out_t == 0 )
        return false;

    if ( !PixelReader::supports(input) )
    {
        OE_WARN << LC << "resizeImage: unsupported format" << std::endl;
        return false;
    }

    if ( output.valid() && !PixelWriter::supports(output.get()) )
    {
        OE_WARN << LC << "resizeImage: pre-allocated output image is in an unsupported format" << std::endl;
        return false;
    }

    unsigned int in_s = input->s();
    unsigned int in_t = input->t();

    if ( !output.valid() )
    {
        output = new osg::Image();

        if ( PixelWriter::supports(input) )
        {
            output->allocateImage( out_s, out_t, input->r(), input->getPixelFormat(), input->getDataType(), input->getPacking() );
            output->setInternalTextureFormat( input->getInternalTextureFormat() );
        }
        else
        {
            // for unsupported write formats, convert to RGBA8 automatically.
            output->allocateImage( out_s, out_t, input->r(), GL_RGBA, GL_UNSIGNED_BYTE );
            output->setInternalTextureFormat( GL_RGB8A_INTERNAL );
        }
    }
    else
    {
        // make sure they match up
        output->setInternalTextureFormat( input->getInternalTextureFormat() );
    }

    if ( in_s == out_s && in_t == out_t && mipmapLevel == 0 && input->getInternalTextureFormat() == output->getInternalTextureFormat() )
    {
        memcpy( output->data(), input->data(), input->getTotalSizeInBytes() );
    }
    else
    {
        PixelReader read( input );
        PixelWriter write( output.get() );

        unsigned int pixel_size_bytes = input->getRowSizeInBytes() / in_s;

        unsigned char* dataOffset = output->getMipmapData(mipmapLevel);
        unsigned int   dataRowSizeBytes = output->getRowSizeInBytes() >> mipmapLevel;

        for( unsigned int output_row=0; output_row < out_t; output_row++ )
        {
            // get an appropriate input row
            float output_row_ratio = (float)output_row/(float)out_t;
            float input_row = output_row_ratio * (float)in_t;
            if ( input_row >= input->t() ) input_row = in_t-1;
            else if ( input_row < 0 ) input_row = 0;

            for( unsigned int output_col = 0; output_col < out_s; output_col++ )
            {
                float output_col_ratio = (float)output_col/(float)out_s;
                float input_col =  output_col_ratio * (float)in_s;
                if ( input_col >= (int)in_s ) input_col = in_s-1;
                else if ( input_col < 0 ) input_col = 0.0f;                

                osg::Vec4 color;

                for(int layer=0; layer<input->r(); ++layer)
                {
                    if (bilinear)
                    {
                        // Do a billinear interpolation for the image
                        int rowMin = osg::maximum((int)floor(input_row), 0);
                        int rowMax = osg::maximum(osg::minimum((int)ceil(input_row), (int)(input->t()-1)), 0);
                        int colMin = osg::maximum((int)floor(input_col), 0);
                        int colMax = osg::maximum(osg::minimum((int)ceil(input_col), (int)(input->s()-1)), 0);                    

                        if (rowMin > rowMax) rowMin = rowMax;
                        if (colMin > colMax) colMin = colMax;  

                        osg::Vec4 urColor = read(colMax, rowMax, layer);
                        osg::Vec4 llColor = read(colMin, rowMin, layer);
                        osg::Vec4 ulColor = read(colMin, rowMax, layer);
                        osg::Vec4 lrColor = read(colMax, rowMin, layer);
                    
                        if ((colMax == colMin) && (rowMax == rowMin))
                        {
                            // Exact value
                            color = urColor;
                        }
                        else if (colMax == colMin)
                        {                     
                            // Linear interpolate vertically            
                            color = llColor * ((double)rowMax - input_row) + ulColor * (input_row - (double)rowMin);
                        }
                        else if (rowMax == rowMin)
                        {                     
                            // Linear interpolate horizontally
                            color = llColor * ((double)colMax - input_col) + lrColor * (input_col - (double)colMin);
                        }
                        else
                        {                        
                            // Bilinear interpolate
                            osg::Vec4 r1 = llColor * ((double)colMax - input_col) + lrColor * (input_col - (double)colMin);
                            osg::Vec4 r2 = ulColor * ((double)colMax - input_col) + urColor * (input_col - (double)colMin);                      
                            color = r1 * ((double)rowMax - input_row) + r2 * (input_row - (double)rowMin);
                        }                         
                    }
                    else
                    {
                        color = read( (int)input_col, (int)input_row, layer ); // read pixel from mip level 0
                    }

                    write( color, output_col, output_row, layer, mipmapLevel ); // write to target mip level
                }
            }
        }
    }

    return true;
}

osg::Image*
ImageUtils::createMipmapBlendedImage( const osg::Image* primary, const osg::Image* secondary )
{
    // ASSUMPTION: primary and secondary are the same size, same format.

    // first, build the image that will hold all the mipmap levels.
    int numMipmapLevels = osg::Image::computeNumberOfMipmapLevels( primary->s(), primary->t() );
    int pixelSizeBytes  = osg::Image::computeRowWidthInBytes( primary->s(), primary->getPixelFormat(), primary->getDataType(), primary->getPacking() ) / primary->s();
    int totalSizeBytes  = 0;
    std::vector< unsigned int > mipmapDataOffsets;

    mipmapDataOffsets.reserve( numMipmapLevels-1 );

    for( int i=0; i<numMipmapLevels; ++i )
    {
        if ( i > 0 )
            mipmapDataOffsets.push_back( totalSizeBytes );

        int level_s = primary->s() >> i;
        int level_t = primary->t() >> i;
        int levelSizeBytes = level_s * level_t * pixelSizeBytes;

        totalSizeBytes += levelSizeBytes;
    }

    unsigned char* data = new unsigned char[totalSizeBytes];

    osg::ref_ptr<osg::Image> result = new osg::Image();
    result->setImage(
        primary->s(), primary->t(), 1,
        primary->getInternalTextureFormat(), 
        primary->getPixelFormat(), 
        primary->getDataType(), 
        data, osg::Image::USE_NEW_DELETE );

    result->setMipmapLevels( mipmapDataOffsets );

    // now, populate the image levels.
    int level_s = primary->s();
    int level_t = primary->t();

    for( int level=0; level<numMipmapLevels; ++level )
    {
        if ( secondary && level > 0 )
            ImageUtils::resizeImage( secondary, level_s, level_t, result, level );
        else
            ImageUtils::resizeImage( primary, level_s, level_t, result, level );

        level_s >>= 1;
        level_t >>= 1;
    }

    return result.release();
}

namespace
{
    struct MixImage
    {
        float _a;
        bool _srcHasAlpha, _destHasAlpha;

        bool operator()( const osg::Vec4f& src, osg::Vec4f& dest )
        {
            float sa = _srcHasAlpha ? _a * src.a() : _a;
            float da = _destHasAlpha ? dest.a() : 1.0f;
            dest.set(
                dest.r()*(1.0f-sa) + src.r()*sa,
                dest.g()*(1.0f-sa) + src.g()*sa,
                dest.b()*(1.0f-sa) + src.b()*sa,
                osg::maximum(sa, da) );             
            return true;
        }
    };
}

bool
ImageUtils::mix(osg::Image* dest, const osg::Image* src, float a)
{
    if (!dest || !src || dest->s() != src->s() || dest->t() != src->t() || src->r() != dest->r() ||
        !PixelReader::supports(src) ||
        !PixelWriter::supports(dest) )
    {
        return false;
    }
    
    PixelVisitor<MixImage> mixer;
    mixer._a = osg::clampBetween( a, 0.0f, 1.0f );
    mixer._srcHasAlpha = src->getPixelSizeInBits() == 32;
    mixer._destHasAlpha = src->getPixelSizeInBits() == 32;

    mixer.accept( src, dest );  

    return true;
}

osg::Image*
ImageUtils::cropImage(const osg::Image* image,
                      double src_minx, double src_miny, double src_maxx, double src_maxy,
                      double &dst_minx, double &dst_miny, double &dst_maxx, double &dst_maxy)
{
    if ( image == 0L )
        return 0L;

    //Compute the desired cropping rectangle
    int windowX        = osg::clampBetween( (int)floor( (dst_minx - src_minx) / (src_maxx - src_minx) * (double)image->s()), 0, image->s()-1);
    int windowY        = osg::clampBetween( (int)floor( (dst_miny - src_miny) / (src_maxy - src_miny) * (double)image->t()), 0, image->t()-1);
    int windowWidth    = osg::clampBetween( (int)ceil(  (dst_maxx - src_minx) / (src_maxx - src_minx) * (double)image->s()) - windowX, 0, image->s());
    int windowHeight   = osg::clampBetween( (int)ceil(  (dst_maxy - src_miny) / (src_maxy - src_miny) * (double)image->t()) - windowY, 0, image->t());    

    if (windowX + windowWidth > image->s())
    {
        windowWidth = image->s() - windowX;        
    }

    if (windowY + windowHeight > image->t())
    {
        windowHeight = image->t() - windowY;
    }
    
    if ((windowWidth * windowHeight) == 0)
    {
        return NULL;
    }

    //Compute the actual bounds of the area we are computing
    double res_s = (src_maxx - src_minx) / (double)image->s();
    double res_t = (src_maxy - src_miny) / (double)image->t();

    dst_minx = src_minx + (double)windowX * res_s;
    dst_miny = src_miny + (double)windowY * res_t;
    dst_maxx = dst_minx + (double)windowWidth * res_s;
    dst_maxy = dst_miny + (double)windowHeight * res_t;

    //OE_NOTICE << "Copying from " << windowX << ", " << windowY << ", " << windowWidth << ", " << windowHeight << std::endl;

    //Allocate the croppped image
    osg::Image* cropped = new osg::Image;
    cropped->allocateImage(windowWidth, windowHeight, image->r(), image->getPixelFormat(), image->getDataType());
    cropped->setInternalTextureFormat( image->getInternalTextureFormat() );
    
    
    for (int layer=0; layer<image->r(); ++layer)
    {
        for (int src_row = windowY, dst_row=0; dst_row < windowHeight; src_row++, dst_row++)
        {
            if (src_row > image->t()-1) OE_NOTICE << "HeightBroke" << std::endl;
            const void* src_data = image->data(windowX, src_row, layer);
            void* dst_data = cropped->data(0, dst_row, layer);
            memcpy( dst_data, src_data, cropped->getRowSizeInBytes());
        }
    }
    return cropped;
}

bool
ImageUtils::isPowerOfTwo(const osg::Image* image)
{
    return (((image->s() & (image->s()-1))==0) &&
            ((image->t() & (image->t()-1))==0));
}


osg::Image*
ImageUtils::createSharpenedImage( const osg::Image* input )
{
    int filter[9] = { 0, -1, 0, -1, 5, -1, 0, -1, 0 };
    osg::Image* output = ImageUtils::cloneImage(input);
    for( int r=0; r<input->r(); ++r)
    {
        for( int t=1; t<input->t()-1; t++ )
        {
            for( int s=1; s<input->s()-1; s++ )
            {
                int pixels[9] = {
                    *(int*)input->data(s-1,t-1,r), *(int*)input->data(s,t-1,r), *(int*)input->data(s+1,t-1,r),
                    *(int*)input->data(s-1,t  ,r), *(int*)input->data(s,t  ,r), *(int*)input->data(s+1,t  ,r),
                    *(int*)input->data(s-1,t+1,r), *(int*)input->data(s,t+1,r), *(int*)input->data(s+1,t+1,r) };

                int shifts[4] = { 0, 8, 16, 32 };

                for( int c=0; c<4; c++ ) // components
                {
                    int mask = 0xff << shifts[c];
                    int sum = 0;
                    for( int i=0; i<9; i++ )
                    {
                        sum += ((pixels[i] & mask) >> shifts[c]) * filter[i];
                    }
                    sum = sum > 255? 255 : sum < 0? 0 : sum;
                    output->data(s,t,r)[c] = sum;
                }
            }
        }
    }
    return output;
}

namespace
{
    static Threading::Mutex         s_emptyImageMutex;
    static osg::ref_ptr<osg::Image> s_emptyImage;
}

osg::Image*
ImageUtils::createEmptyImage()
{
    if (!s_emptyImage.valid())
    {
        Threading::ScopedMutexLock exclusive( s_emptyImageMutex );
        if (!s_emptyImage.valid())
        {            
            s_emptyImage = createEmptyImage( 1, 1 );
        }     
    }
    return s_emptyImage.get();
}

osg::Image*
ImageUtils::createEmptyImage(unsigned int s, unsigned int t)
{
    osg::Image* empty = new osg::Image;
    empty->allocateImage(s,t,1, GL_RGBA, GL_UNSIGNED_BYTE);
    empty->setInternalTextureFormat( GL_RGB8A_INTERNAL );
    unsigned char *data = empty->data(0,0);
    memset(data, 0, 4 * s * t);
    return empty;
}

bool
ImageUtils::isEmptyImage(const osg::Image* image, float alphaThreshold)
{
    if ( !hasAlphaChannel(image) || !PixelReader::supports(image) )
        return false;

    PixelReader read(image);
    for(unsigned r=0; r<(unsigned)image->r(); ++r)
    {
        for(unsigned t=0; t<(unsigned)image->t(); ++t) 
        {
            for(unsigned s=0; s<(unsigned)image->s(); ++s)
            {
                osg::Vec4 color = read(s, t, r);
                if ( color.a() > alphaThreshold )
                    return false;
            }
        }
    }
    return true;
}


osg::Image*
ImageUtils::createOnePixelImage(const osg::Vec4& color)
{
    osg::Image* image = new osg::Image;
    image->allocateImage(1,1,1, GL_RGBA, GL_UNSIGNED_BYTE);
    image->setInternalTextureFormat( GL_RGB8A_INTERNAL );
    PixelWriter write(image);
    write(color, 0, 0);
    return image;
}

bool
ImageUtils::isSingleColorImage(const osg::Image* image, float threshold)
{
    if ( !PixelReader::supports(image) )
        return false;

    PixelReader read(image);

    osg::Vec4 referenceColor = read(0, 0, 0);
    float refR = referenceColor.r();
    float refG = referenceColor.g();
    float refB = referenceColor.b();
    float refA = referenceColor.a();

    for(unsigned r=0; r<(unsigned)image->r(); ++r)
    {
        for(unsigned t=0; t<(unsigned)image->t(); ++t) 
        {
            for(unsigned s=0; s<(unsigned)image->s(); ++s)
            {
                osg::Vec4 color = read(s, t, r);
                if (   (fabs(color.r()-refR) > threshold)
                    || (fabs(color.g()-refG) > threshold)
                    || (fabs(color.b()-refB) > threshold)
                    || (fabs(color.a()-refA) > threshold) )
                {
                    return false;
                }
            }
        }
    }
    return true;
}

bool
ImageUtils::canConvert( const osg::Image* image, GLenum pixelFormat, GLenum dataType )
{
    if ( !image ) return false;
    return PixelReader::supports( image ) && PixelWriter::supports(pixelFormat, dataType);
}

osg::Image*
ImageUtils::convert(const osg::Image* image, GLenum pixelFormat, GLenum dataType)
{
    if ( !image )
        return 0L;

    if ( image->getPixelFormat() == pixelFormat && image->getDataType() == dataType)
    {
        GLenum texFormat = image->getInternalTextureFormat();
        if (dataType != GL_UNSIGNED_BYTE
            || (pixelFormat == GL_RGB  && texFormat == GL_RGB8_INTERNAL)
            || (pixelFormat == GL_RGBA && texFormat == GL_RGB8A_INTERNAL))
        return cloneImage(image);
    }
    if ( !canConvert(image, pixelFormat, dataType) )
        return 0L;

    osg::Image* result = new osg::Image();
    result->allocateImage(image->s(), image->t(), image->r(), pixelFormat, dataType);
    memset(result->data(), 0, result->getTotalSizeInBytes());

    if ( pixelFormat == GL_RGB && dataType == GL_UNSIGNED_BYTE )
        result->setInternalTextureFormat( GL_RGB8_INTERNAL );
    else if ( pixelFormat == GL_RGBA && dataType == GL_UNSIGNED_BYTE )
        result->setInternalTextureFormat( GL_RGB8A_INTERNAL );
    else
        result->setInternalTextureFormat( pixelFormat );

    PixelVisitor<CopyImage>().accept( image, result );

    return result;
}

osg::Image*
ImageUtils::convertToRGB8(const osg::Image *image)
{
    return convert( image, GL_RGB, GL_UNSIGNED_BYTE );
}

osg::Image*
ImageUtils::convertToRGBA8(const osg::Image* image)
{
    return convert( image, GL_RGBA, GL_UNSIGNED_BYTE );
}

bool 
ImageUtils::areEquivalent(const osg::Image *lhs, const osg::Image *rhs)
{
    if (lhs == rhs) return true;

    if ((lhs->s() == rhs->s()) &&
        (lhs->t() == rhs->t()) &&
        (lhs->r() == rhs->r()) &&
        (lhs->getInternalTextureFormat() == rhs->getInternalTextureFormat()) &&
        (lhs->getPixelFormat() == rhs->getPixelFormat()) &&
        (lhs->getDataType() == rhs->getDataType()) &&
        (lhs->getPacking() == rhs->getPacking()) &&
        (lhs->getImageSizeInBytes() == rhs->getImageSizeInBytes()))
    {
        unsigned int size = lhs->getImageSizeInBytes();
        const unsigned char* ptr1 = lhs->data();
        const unsigned char* ptr2 = rhs->data();
        for (unsigned int i = 0; i < size; ++i)
        {
            if ( *ptr1++ != *ptr2++ )
                return false;
        }

        return true;
    }

    return false;
}

bool
ImageUtils::hasAlphaChannel(const osg::Image* image) 
{
    return image && (
        image->getPixelFormat() == GL_RGBA ||
        image->getPixelFormat() == GL_BGRA ||
        image->getPixelFormat() == GL_LUMINANCE_ALPHA ||
        image->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
        image->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
        image->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ||
        image->getPixelFormat() == GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG ||
        image->getPixelFormat() == GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG );
}


bool
ImageUtils::hasTransparency(const osg::Image* image, float threshold)
{
    if ( !image || !PixelReader::supports(image) )
        return false;

    PixelReader read(image);
    for( int r=0; r<image->r(); ++r)
        for( int t=0; t<image->t(); ++t )
            for( int s=0; s<image->s(); ++s )
                if ( read(s, t, r).a() < threshold )
                    return true;

    return false;
}


bool
ImageUtils::featherAlphaRegions(osg::Image* image, float maxAlpha)
{
    if ( !PixelReader::supports(image) || !PixelWriter::supports(image) )
        return false;

    PixelReader read (image);
    PixelWriter write(image);

    int ns = image->s();
    int nt = image->t();
    int nr = image->r();

    osg::Vec4 n;

    for( int r=0; r<nr; ++r )
    {
        for( int t=0; t<nt; ++t )
        {
            bool rowdone = false;
            for( int s=0; s<ns && !rowdone; ++s )
            {
                osg::Vec4 pixel = read(s, t, r);
                if ( pixel.a() <= maxAlpha )
                {
                    bool wrote = false;
                    if ( s < ns-1 ) {
                        n = read( s+1, t, r);
                        if ( n.a() > maxAlpha ) {
                            write( n, s, t, r);
                            wrote = true;
                        }
                    }
                    if ( !wrote && s > 0 ) {
                        n = read( s-1, t, r);
                        if ( n.a() > maxAlpha ) {
                            write( n, s, t, r);
                            rowdone = true;
                        }
                    }
                }
            }
        }

        for( int s=0; s<ns; ++s )
        {
            bool coldone = false;
            for( int t=0; t<nt && !coldone; ++t )
            {
                osg::Vec4 pixel = read(s, t, r);
                if ( pixel.a() <= maxAlpha )
                {
                    bool wrote = false;
                    if ( t < nt-1 ) {
                        n = read( s, t+1, r );
                        if ( n.a() > maxAlpha ) {
                            write( n, s, t, r );
                            wrote = true;
                        }
                    }
                    if ( !wrote && t > 0 ) {
                        n = read( s, t-1, r );
                        if ( n.a() > maxAlpha ) {
                            write( n, s, t, r);
                            coldone = true;
                        }
                    }
                }
            }
        }
    }

    return true;
}


bool
ImageUtils::convertToPremultipliedAlpha(osg::Image* image)
{
    if ( !PixelReader::supports(image) || !PixelWriter::supports(image) )
        return false;

    PixelReader read(image);
    PixelWriter write(image);
    for(int r=0; r<image->r(); ++r) {
        for(int s=0; s<image->s(); ++s) {
            for( int t=0; t<image->t(); ++t ) {
                osg::Vec4f c = read(s, t, r);
                write( osg::Vec4f(c.r()*c.a(), c.g()*c.a(), c.b()*c.a(), c.a()), s, t, r);
            }
        }
    }
    return true;
}


bool
ImageUtils::isCompressed(const osg::Image *image)
{
    //Later versions of OSG have an Image::isCompressed function but earlier versions like 2.8.3 do not.  This is a workaround so that 
    //we can tell if an image is compressed on all versions of OSG.
    switch(image->getPixelFormat())
    {
        case(GL_COMPRESSED_ALPHA_ARB):
        case(GL_COMPRESSED_INTENSITY_ARB):
        case(GL_COMPRESSED_LUMINANCE_ALPHA_ARB):
        case(GL_COMPRESSED_LUMINANCE_ARB):
        case(GL_COMPRESSED_RGBA_ARB):
        case(GL_COMPRESSED_RGB_ARB):
        case(GL_COMPRESSED_RGB_S3TC_DXT1_EXT):
        case(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT):
        case(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT):
        case(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT):
        case(GL_COMPRESSED_SIGNED_RED_RGTC1_EXT):
        case(GL_COMPRESSED_RED_RGTC1_EXT):
        case(GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT):
        case(GL_COMPRESSED_RED_GREEN_RGTC2_EXT):
        case(GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG): 
        case(GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG):
        case(GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG):
        case(GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG):
            return true;
        default:
            return false;
    }
}


bool
ImageUtils::isFloatingPointInternalFormat(GLint i)
{
    return 
        (i >= 0x8C10 && i <= 0x8C17) || // GL_TEXTURE_RED_TYPE_ARB, et al
        (i >= 0x8814 && i <= 0x881F);   // GL_RGBA32F_ARB, et al
}

//------------------------------------------------------------------------

namespace
{
    //static const float r10= 1.0f/1023.0f;
    //static const float r8 = 1.0f/255.0f;
    //static const float r6 = 1.0f/63.0f;
    static const float r5 = 1.0f/31.0f;
    //static const float r4 = 1.0f/15.0f;
    static const float r3 = 1.0f/7.0f;
    static const float r2 = 1.0f/3.0f;

    // The scale factors to convert from an image data type to a
    // float. This is copied from OSG; I think the factors for the signed
    // types are wrong, but need to investigate further.

    template<typename T> struct GLTypeTraits;

    template<> struct GLTypeTraits<GLbyte>
    {
        static float scale() { return 1.0f/128.0f; } // XXX
    };

    template<> struct GLTypeTraits<GLubyte>
    {
        static float scale() { return 1.0f/255.0f; }
    };

    template<> struct GLTypeTraits<GLshort>
    {
        static float scale() { return 1.0f/32768.0f; } // XXX
    };

    template<> struct GLTypeTraits<GLushort>
    {
        static float scale() { return 1.0f/65535.0f; }
    };

    template<> struct GLTypeTraits<GLint>
    {
        static float scale() { return 1.0f/2147483648.0f; } // XXX
    };

    template<> struct GLTypeTraits<GLuint>
    {
        static float scale() { return 1.0f/4294967295.0f; }
    };

    template<> struct GLTypeTraits<GLfloat>
    {
        static float scale() { return 1.0f; }
    };

    // The Reader function that performs the read.
    template<int Format, typename T> struct ColorReader;
    template<int Format, typename T> struct ColorWriter;

    template<typename T>
    struct ColorReader<GL_DEPTH_COMPONENT, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float l = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(l, l, l, 1.0f);
        }
    };

    template<typename T>
    struct ColorWriter<GL_DEPTH_COMPONENT, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m)
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            (*ptr) = (T)(c.r() / GLTypeTraits<T>::scale());
        }
    };

    template<typename T>
    struct ColorReader<GL_LUMINANCE, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float l = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(l, l, l, 1.0f);
        }
    };

    template<typename T>
    struct ColorWriter<GL_LUMINANCE, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m)
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            (*ptr) = (T)(c.r() / GLTypeTraits<T>::scale());
        }
    };

    template<typename T>
    struct ColorReader<GL_ALPHA, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float a = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(1.0f, 1.0f, 1.0f, a);
        }
    };

    template<typename T>
    struct ColorWriter<GL_ALPHA, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m)
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            (*ptr) = (T)(c.a() / GLTypeTraits<T>::scale());
        }
    };

    template<typename T>
    struct ColorReader<GL_LUMINANCE_ALPHA, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float l = float(*ptr++) * GLTypeTraits<T>::scale();
            float a = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(l, l, l, a);
        }
    };

    template<typename T>
    struct ColorWriter<GL_LUMINANCE_ALPHA, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            *ptr++ = (T)( c.r() / GLTypeTraits<T>::scale() );
            *ptr   = (T)( c.a() / GLTypeTraits<T>::scale() );
        }
    };

    template<typename T>
    struct ColorReader<GL_RGB, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float d = float(*ptr++) * GLTypeTraits<T>::scale();
            float g = float(*ptr++) * GLTypeTraits<T>::scale();
            float b = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(d, g, b, 1.0f);
        }
    };

    template<typename T>
    struct ColorWriter<GL_RGB, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            *ptr++ = (T)( c.r() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.g() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.b() / GLTypeTraits<T>::scale() );
        }
    };

    template<typename T>
    struct ColorReader<GL_RGBA, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float d = float(*ptr++) * GLTypeTraits<T>::scale();
            float g = float(*ptr++) * GLTypeTraits<T>::scale();
            float b = float(*ptr++) * GLTypeTraits<T>::scale();
            float a = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(d, g, b, a);
        }
    };

    template<typename T>
    struct ColorWriter<GL_RGBA, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m)
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            *ptr++ = (T)( c.r() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.g() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.b() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.a() / GLTypeTraits<T>::scale() );
        }
    };

    template<typename T>
    struct ColorReader<GL_BGR, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float b = float(*ptr) * GLTypeTraits<T>::scale();
            float g = float(*ptr++) * GLTypeTraits<T>::scale();
            float d = float(*ptr++) * GLTypeTraits<T>::scale();
            return osg::Vec4(d, g, b, 1.0f);
        }
    };

    template<typename T>
    struct ColorWriter<GL_BGR, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            *ptr++ = (T)( c.b() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.g() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.r() / GLTypeTraits<T>::scale() );
        }
    };

    template<typename T>
    struct ColorReader<GL_BGRA, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            const T* ptr = (const T*)ia->data(s, t, r, m);
            float b = float(*ptr++) * GLTypeTraits<T>::scale();
            float g = float(*ptr++) * GLTypeTraits<T>::scale();
            float d = float(*ptr++) * GLTypeTraits<T>::scale();
            float a = float(*ptr) * GLTypeTraits<T>::scale();
            return osg::Vec4(d, g, b, a);
        }
    };

    template<typename T>
    struct ColorWriter<GL_BGRA, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            T* ptr = (T*)iw->data(s, t, r, m);
            *ptr++ = (T)( c.b() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.g() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.r() / GLTypeTraits<T>::scale() );
            *ptr++ = (T)( c.a() / GLTypeTraits<T>::scale() );
        }
    };

    template<typename T>
    struct ColorReader<0, T>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            return osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    };

    template<typename T>
    struct ColorWriter<0, T>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            //nop
        }
    };

    template<>
    struct ColorReader<GL_UNSIGNED_SHORT_5_5_5_1, GLushort>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
            GLushort p = *(const GLushort*)ia->data(s, t, r, m);
            //internal format GL_RGB5_A1 is implied
            return osg::Vec4( r5*(float)(p>>11), r5*(float)((p&0x7c0)>>6), r5*((p&0x3e)>>1), (float)(p&0x1));
        }
    };

    template<>
    struct ColorWriter<GL_UNSIGNED_SHORT_5_5_5_1, GLushort>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            GLushort
                red = (unsigned short)(c.r()*255),
                g = (unsigned short)(c.g()*255),
                b = (unsigned short)(c.b()*255),
                a = c.a() < 0.15 ? 0 : 1;

            GLushort* ptr = (GLushort*)iw->data(s, t, r, m);
            *ptr = (((red) & (0xf8)) << 8) | (((g) & (0xf8)) << 3) | (((b) & (0xF8)) >> 2) | a;
        }
    };

    template<>
    struct ColorReader<GL_UNSIGNED_BYTE_3_3_2, GLubyte>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* ia, int s, int t, int r, int m)
        {
              GLubyte p = *(const GLubyte*)ia->data(s,t,r,m);
            // internal format GL_R3_G3_B2 is implied
            return osg::Vec4( r3*(float)(p>>5), r3*(float)((p&0x28)>>2), r2*(float)(p&0x3), 1.0f );
        }
    };

    template<>
    struct ColorWriter<GL_UNSIGNED_BYTE_3_3_2, GLubyte>
    {
        static void write(const ImageUtils::PixelWriter* iw, const osg::Vec4f& c, int s, int t, int r, int m )
        {
            GLubyte* ptr = (GLubyte*)iw->data(s,t,r,m);
            OE_WARN << LC << "Target GL_UNSIGNED_BYTE_3_3_2 not yet implemented" << std::endl;
        }
    };

    template<>
    struct ColorReader<GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GLubyte>
    {
        static osg::Vec4 read(const ImageUtils::PixelReader* pr, int s, int t, int r, int m)
        {
            static const int BLOCK_BYTES = 8;

            unsigned int blocksPerRow = pr->_image->s()/4;
            unsigned int bs = s/4, bt = t/4;
            unsigned int blockStart = (bt*blocksPerRow+bs) * BLOCK_BYTES;

            const GLushort* p = (const GLushort*)(pr->data() + blockStart);

            GLushort c0p = *p++;
            osg::Vec4f c0(
                (float)(c0p >> 11)/31.0f,
                (float)((c0p & 0x07E0) >> 5)/63.0f,
                (float)((c0p & 0x001F))/31.0f,
                1.0f );

            GLushort c1p = *p++;
            osg::Vec4f c1(
                (float)(c1p >> 11)/31.0f,
                (float)((c1p & 0x07E0) >> 5)/63.0f,
                (float)((c1p & 0x001F))/31.0f,
                1.0f );

            static const float one_third  = 1.0f/3.0f;
            static const float two_thirds = 2.0f/3.0f;

            osg::Vec4f c2, c3;
            if ( c0p > c1p )
            {
                c2 = c0*two_thirds + c1*one_third;
                c3 = c0*one_third  + c1*two_thirds;
            }
            else
            {
                c2 = c0*0.5 + c1*0.5;
                c3.set(0,0,0,1);
            }

            unsigned int table = *(unsigned int*)p;
            int ls = s-4*bs, lt = t-4*bt; //int ls = s % 4, lt = t % 4;
            int x = ls + (4 * lt);

            unsigned int index = (table >> (2*x)) & 0x00000003;

            return index==0? c0 : index==1? c1 : index==2? c2 : c3;
        }
    };

    template<int GLFormat>
    inline ImageUtils::PixelReader::ReaderFunc
    chooseReader(GLenum dataType)
    {
        switch (dataType)
        {
        case GL_BYTE:
            return &ColorReader<GLFormat, GLbyte>::read;
        case GL_UNSIGNED_BYTE:
            return &ColorReader<GLFormat, GLubyte>::read;
        case GL_SHORT:
            return &ColorReader<GLFormat, GLshort>::read;
        case GL_UNSIGNED_SHORT:
            return &ColorReader<GLFormat, GLushort>::read;
        case GL_INT:
            return &ColorReader<GLFormat, GLint>::read;
        case GL_UNSIGNED_INT:
            return &ColorReader<GLFormat, GLuint>::read;
        case GL_FLOAT:
            return &ColorReader<GLFormat, GLfloat>::read;       
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return &ColorReader<GL_UNSIGNED_SHORT_5_5_5_1, GLushort>::read;
        case GL_UNSIGNED_BYTE_3_3_2:
            return &ColorReader<GL_UNSIGNED_BYTE_3_3_2, GLubyte>::read;
        default:
            return &ColorReader<0, GLbyte>::read;
        }
    }

    inline ImageUtils::PixelReader::ReaderFunc
    getReader( GLenum pixelFormat, GLenum dataType )
    {
        switch( pixelFormat )
        {
        case GL_DEPTH_COMPONENT:
            return chooseReader<GL_DEPTH_COMPONENT>(dataType);
            break;
        case GL_LUMINANCE:
            return chooseReader<GL_LUMINANCE>(dataType);
            break;        
        case GL_ALPHA:
            return chooseReader<GL_ALPHA>(dataType);
            break;        
        case GL_LUMINANCE_ALPHA:
            return chooseReader<GL_LUMINANCE_ALPHA>(dataType);
            break;        
        case GL_RGB:
            return chooseReader<GL_RGB>(dataType);
            break;        
        case GL_RGBA:
            return chooseReader<GL_RGBA>(dataType);
            break;        
        case GL_BGR:
            return chooseReader<GL_BGR>(dataType);
            break;        
        case GL_BGRA:
            return chooseReader<GL_BGRA>(dataType);
            break; 
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            return &ColorReader<GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GLubyte>::read;
            break;
        default:
            return 0L;
            break;
        }
    }
}
    
ImageUtils::PixelReader::PixelReader(const osg::Image* image) :
_image(image)
{
    _colMult = _image->getPixelSizeInBits() / 8;
    _rowMult = _image->getRowSizeInBytes();
    _imageSize = _image->getImageSizeInBytes();
    GLenum dataType = _image->getDataType();
    _reader = getReader( _image->getPixelFormat(), dataType );
    if ( !_reader )
    {
        OE_WARN << "[PixelReader] No reader found for pixel format " << std::hex << _image->getPixelFormat() << std::endl; 
        _reader = &ColorReader<0,GLbyte>::read;
    }
}

bool
ImageUtils::PixelReader::supports( GLenum pixelFormat, GLenum dataType )
{
    return getReader(pixelFormat, dataType) != 0L;
}

//------------------------------------------------------------------------

namespace
{
    template<int GLFormat>
    inline ImageUtils::PixelWriter::WriterFunc chooseWriter(GLenum dataType)
    {
        switch (dataType)
        {
        case GL_BYTE:
            return &ColorWriter<GLFormat, GLbyte>::write;
        case GL_UNSIGNED_BYTE:
            return &ColorWriter<GLFormat, GLubyte>::write;
        case GL_SHORT:
            return &ColorWriter<GLFormat, GLshort>::write;
        case GL_UNSIGNED_SHORT:
            return &ColorWriter<GLFormat, GLushort>::write;
        case GL_INT:
            return &ColorWriter<GLFormat, GLint>::write;
        case GL_UNSIGNED_INT:
            return &ColorWriter<GLFormat, GLuint>::write;
        case GL_FLOAT:
            return &ColorWriter<GLFormat, GLfloat>::write;       
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return &ColorWriter<GL_UNSIGNED_SHORT_5_5_5_1, GLushort>::write;
        case GL_UNSIGNED_BYTE_3_3_2:
            return &ColorWriter<GL_UNSIGNED_BYTE_3_3_2, GLubyte>::write;
        default:
            return 0L;
        }
    }

    inline ImageUtils::PixelWriter::WriterFunc getWriter(GLenum pixelFormat, GLenum dataType)
    {
        switch( pixelFormat )
        {
        case GL_DEPTH_COMPONENT:
            return chooseWriter<GL_DEPTH_COMPONENT>(dataType);
            break;
        case GL_LUMINANCE:
            return chooseWriter<GL_LUMINANCE>(dataType);
            break;        
        case GL_ALPHA:
            return chooseWriter<GL_ALPHA>(dataType);
            break;        
        case GL_LUMINANCE_ALPHA:
            return chooseWriter<GL_LUMINANCE_ALPHA>(dataType);
            break;        
        case GL_RGB:
            return chooseWriter<GL_RGB>(dataType);
            break;        
        case GL_RGBA:
            return chooseWriter<GL_RGBA>(dataType);
            break;        
        case GL_BGR:
            return chooseWriter<GL_BGR>(dataType);
            break;        
        case GL_BGRA:
            return chooseWriter<GL_BGRA>(dataType);
            break; 
        default:
            return 0L;
            break;
        }
    }
}
    
ImageUtils::PixelWriter::PixelWriter(osg::Image* image) :
_image(image)
{
    _colMult = _image->getPixelSizeInBits() / 8;
    _rowMult = _image->getRowSizeInBytes();
    _imageSize = _image->getImageSizeInBytes();
    GLenum dataType = _image->getDataType();
    _writer = getWriter( _image->getPixelFormat(), dataType );
    if ( !_writer )
    {
        OE_WARN << "[PixelWriter] No writer found for pixel format " << std::hex << _image->getPixelFormat() << std::endl; 
        _writer = &ColorWriter<0, GLbyte>::write;
    }
}

bool
ImageUtils::PixelWriter::supports( GLenum pixelFormat, GLenum dataType )
{
    return getWriter(pixelFormat, dataType) != 0L;
}
