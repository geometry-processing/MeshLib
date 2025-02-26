#include "MRDicom.h"
#ifndef MRVOXELS_NO_DICOM
#include "MRObjectVoxels.h"
#include "MRScanHelpers.h"
#include "MRVDBConversions.h"
#include "MRVoxelsLoad.h"
#include "MRVoxelsSave.h"

#include "MRMesh/MRDirectory.h"
#include "MRMesh/MRParallelFor.h"
#include "MRMesh/MRStringConvert.h"
#include "MRMesh/MRTimer.h"
#include "MRPch/MRSpdlog.h"

#pragma warning(push)
#pragma warning(disable: 4515)
#if _MSC_VER >= 1937 // Visual Studio 2022 version 17.7
#pragma warning(disable: 5267) //definition of implicit copy constructor is deprecated because it has a user-provided destructor
#endif
#include <gdcmImageHelper.h>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmTagKeywords.h>
#pragma warning(pop)

namespace
{

using namespace MR;
using namespace MR::VoxelsLoad;

ScalarType convertToScalarType( const gdcm::PixelFormat& format )
{
    switch ( gdcm::PixelFormat::ScalarType( format ) )
    {
        case gdcm::PixelFormat::UINT8:
            return ScalarType::UInt8;
        case gdcm::PixelFormat::INT8:
            return ScalarType::Int8;
        case gdcm::PixelFormat::UINT16:
            return ScalarType::UInt16;
        case gdcm::PixelFormat::INT16:
            return ScalarType::Int16;
        case gdcm::PixelFormat::UINT32:
            return ScalarType::UInt32;
        case gdcm::PixelFormat::INT32:
            return ScalarType::Int32;
        case gdcm::PixelFormat::UINT64:
            return ScalarType::UInt64;
        case gdcm::PixelFormat::INT64:
            return ScalarType::Int64;
        default:
            return ScalarType::Unknown;
    }
}

template <typename T>
std::pair<gdcm::PixelFormat::ScalarType, gdcm::Tag> getGDCMTypeAndTag()
{
    // https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.24.html
    // https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html
    // https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.25.html

    if constexpr ( std::floating_point<T> )
        static_assert( dependent_false<T>, "GDCM doesn't support floating point DICOMs" );
    else if constexpr ( std::same_as<T, uint16_t> )
        return { gdcm::PixelFormat::ScalarType::UINT16, gdcm::Tag( 0x7FE0, 0x0010 ) };
    else
        static_assert( dependent_false<T>, "Unsupported type T" );
}

} // namespace

namespace MR
{

namespace VoxelsLoad
{

bool isDicomFile( const std::filesystem::path& path, std::string* seriesUid )
{
    std::ifstream ifs( path, std::ios_base::binary );

#ifdef __EMSCRIPTEN__
    // try to detect by ourselves
    // GDCM uses exceptions which causes problems on Wasm
    constexpr auto cDicomMagicNumberOffset = 0x80;
    constexpr std::array cDicomMagicNumber { 'D', 'I', 'C', 'M' };
    // NOTE: std::ifstream::get appends a null character
    std::array<char, std::size( cDicomMagicNumber ) + 1> buf;
    if ( !ifs.seekg( cDicomMagicNumberOffset, std::ios::beg ) || !ifs.get( buf.data(), buf.size(), '\0' ) )
        return false;
    if ( std::strncmp( buf.data(), cDicomMagicNumber.data(), cDicomMagicNumber.size() ) != 0 )
        return false;
    ifs.seekg( 0, std::ios::beg );
    assert( ifs );
#endif

    gdcm::ImageReader ir;
    ir.SetStream( ifs );
    if ( !ir.CanRead() )
        return false;
    // we read these tags to be able to determine whether this file is dicom dir or image
    auto tags = {
        gdcm::Tag( 0x0002, 0x0002 ), // media storage
        gdcm::Tag( 0x0008, 0x0016 ), // media storage
        gdcm::Keywords::PhotometricInterpretation::GetTag(),
        gdcm::Keywords::ImagePositionPatient::GetTag(), // is for image origin in mm,
        gdcm::Keywords::SeriesInstanceUID::GetTag(),
        gdcm::Tag( 0x0028, 0x0010 ),gdcm::Tag( 0x0028, 0x0011 ),gdcm::Tag( 0x0028, 0x0008 )}; // is for dimensions
    if ( !ir.ReadSelectedTags( tags ) )
        return false;
    gdcm::MediaStorage ms;
    ms.SetFromFile( ir.GetFile() );

    // skip unsupported media storage
    if ( ms == gdcm::MediaStorage::MediaStorageDirectoryStorage || ms == gdcm::MediaStorage::SecondaryCaptureImageStorage
        || ms == gdcm::MediaStorage::BasicTextSR )
    {
        spdlog::warn( "DICOM file {} has unsupported media storage {}", utf8string( path ), (int)ms );
        return false;
    }

    // unfortunatly gdcm::ImageHelper::GetPhotometricInterpretationValue returns something even if no data in the file
    if ( !gdcm::ImageHelper::GetPointerFromElement( gdcm::Keywords::PhotometricInterpretation::GetTag(), ir.GetFile() ) )
    {
        spdlog::warn( "DICOM file {} does not have Photometric Interpretation", utf8string( path ) );
        return false;
    }

    auto photometric = gdcm::ImageHelper::GetPhotometricInterpretationValue( ir.GetFile() );
    if ( photometric != gdcm::PhotometricInterpretation::MONOCHROME2 &&
         photometric != gdcm::PhotometricInterpretation::MONOCHROME1 )
    {
        spdlog::warn( "DICOM file {} has Photometric Interpretation other than Monochrome", utf8string( path ) );
        return false;
    }

    auto dims = gdcm::ImageHelper::GetDimensionsValue( ir.GetFile() );
    if ( dims.size() != 3 )
    {
        spdlog::warn( "DICOM file {} has Dimensions Value other than 3", utf8string( path ) );
        return false;
    }

    if ( seriesUid )
    {
        const auto& ds = ir.GetFile().GetDataSet();
        if ( ds.FindDataElement( gdcm::Keywords::SeriesInstanceUID::GetTag() ) )
        {
            const auto& de = ds.GetDataElement( gdcm::Keywords::SeriesInstanceUID::GetTag() );
            gdcm::Keywords::SeriesInstanceUID uid;
            uid.SetFromDataElement( de );
            auto uidVal = uid.GetValue();
            *seriesUid = uidVal;
        }
    }

    return true;
}

struct DCMFileLoadResult
{
    bool success = false;
    float min = +std::numeric_limits<float>::max();
    float max = -std::numeric_limits<float>::max();
    std::string seriesDescription;
    AffineXf3f xf;
};

DCMFileLoadResult loadSingleFile( const std::filesystem::path& path, SimpleVolume& data, size_t offset )
{
    MR_TIMER;
    DCMFileLoadResult res;

    std::ifstream fstr( path, std::ifstream::binary );
    gdcm::ImageReader ir;
    ir.SetStream( fstr );

    if ( !ir.Read() )
    {
        spdlog::error( "Cannot read image from DICOM file {}", utf8string( path ) );
        return res;
    }

    const gdcm::DataSet& ds = ir.GetFile().GetDataSet();
    if( ds.FindDataElement( gdcm::Keywords::SeriesDescription::GetTag() ) )
    {
        const gdcm::DataElement& de = ds.GetDataElement( gdcm::Keywords::SeriesDescription::GetTag() );
        gdcm::Keywords::SeriesDescription desc;
        desc.SetFromDataElement( de );
        auto descVal = desc.GetValue();
        res.seriesDescription = descVal;
    }

    if( ds.FindDataElement( gdcm::Keywords::ImagePositionPatient::GetTag() ) )
    {
        gdcm::DataElement dePosition = ds.GetDataElement( gdcm::Keywords::ImagePositionPatient::GetTag() );
        gdcm::Keywords::ImagePositionPatient atPos;
        atPos.SetFromDataElement( dePosition );
        for (int i = 0; i < 3; ++i) {
            res.xf.b[i] = float( atPos.GetValue( i ) );
        }
        res.xf.b /= 1000.0f;
    }

    if( ds.FindDataElement( gdcm::Keywords::ImageOrientationPatient::GetTag() ) )
    {
        gdcm::DataElement deOri = ds.GetDataElement( gdcm::Keywords::ImageOrientationPatient::GetTag() );
        gdcm::Keywords::ImageOrientationPatient atOri;
        atOri.SetFromDataElement( deOri );
        for (int i = 0; i < 3; ++i)
            res.xf.A.x[i] = float( atOri.GetValue( i ) );
        for (int i = 0; i < 3; ++i)
            res.xf.A.y[i] = float( atOri.GetValue( 3 + i ) );
    }

    res.xf.A.x = res.xf.A.x.normalized();
    res.xf.A.y = res.xf.A.y.normalized();
    res.xf.A.z = cross( res.xf.A.x, res.xf.A.y );
    res.xf.A = res.xf.A.transposed();

    const auto& gimage = ir.GetImage();
    auto dimsNum = gimage.GetNumberOfDimensions();
    const unsigned* dims = gimage.GetDimensions();
    bool needInvertZ = false;

    if ( data.dims.x == 0 || data.dims.y == 0 )
    {
        data.dims.x = dims[0];
        data.dims.y = dims[1];
    }
    if ( dimsNum == 3 )
    {
        data.dims.z = dims[2];
    }
    if ( data.voxelSize[0] == 0.0f )
    {
        const double* spacing = gimage.GetSpacing();
        if ( spacing[0] == 1 && spacing[1] == 1 && spacing[2] == 1 )
        {
            // gdcm was unable to find the spacing, so find it by ourselves
            if( ds.FindDataElement( gdcm::Keywords::PixelSpacing::GetTag() ) )
            {
                const gdcm::DataElement& de = ds.GetDataElement( gdcm::Keywords::PixelSpacing::GetTag() );
                gdcm::Keywords::PixelSpacing desc;
                desc.SetFromDataElement( de );
                data.voxelSize.x = float( desc.GetValue(0) / 1000 );
                data.voxelSize.y = float( desc.GetValue(1) / 1000 );
            }
        }
        else
        {
            data.voxelSize.x = float( spacing[0] / 1000 );
            data.voxelSize.y = float( spacing[1] / 1000 );
        }
        if ( data.voxelSize.z == 0.0f )
        {
            if ( dimsNum == 3 )
            {
                float spacingZ = 0.0f;
                if ( ds.FindDataElement( gdcm::Keywords::SpacingBetweenSlices::GetTag() ) )
                {
                    const gdcm::DataElement& de = ds.GetDataElement( gdcm::Keywords::SpacingBetweenSlices::GetTag() );
                    gdcm::Keywords::SpacingBetweenSlices desc;
                    desc.SetFromDataElement( de );
                    spacingZ = float( desc.GetValue() );
                    // looks like if this tag is set image stored inverted by Z
                    // no other tags was found to determine orientation (compared with cases without this tag)
                    needInvertZ = spacingZ > 0.0f;
                }
                else
                {
                    spacingZ = float( spacing[2] );
                    needInvertZ = spacingZ < 0.0f;
                }
                data.voxelSize.z = std::abs( spacingZ ) * 1e-3f;
            }
            else
                data.voxelSize.z = data.voxelSize.x;
        }
    }
    else if ( data.dims.x != (int) dims[0] || data.dims.y != (int) dims[1] )
    {
        spdlog::error( "loadSingle: dimensions are inconsistent with other files, file: {}", utf8string( path ) );
        return res;
    }
    if ( gimage.GetPhotometricInterpretation() != gdcm::PhotometricInterpretation::MONOCHROME2 &&
         gimage.GetPhotometricInterpretation() != gdcm::PhotometricInterpretation::MONOCHROME1 )
    {
        spdlog::error( "loadSingle: unexpected PhotometricInterpretation, file: {}", utf8string( path ) );
        spdlog::error( "PhotometricInterpretation: {}", (int)gimage.GetPhotometricInterpretation() );
        return res;
    }
    auto min = gimage.GetPixelFormat().GetMin();
    auto max = gimage.GetPixelFormat().GetMax();
    auto pixelSize = gimage.GetPixelFormat().GetPixelSize();
    auto scalarType = convertToScalarType( gimage.GetPixelFormat() );
    auto caster = getTypeConverter( scalarType, max - min, min );
    if ( !caster )
    {
        spdlog::error( "loadSingle: cannot make type converter, file: {}", utf8string( path ) );
        spdlog::error( "Type: {}", (int)gimage.GetPixelFormat() );
        return res;
    }
    std::vector<char> cacheBuffer( gimage.GetBufferLength() );
    if ( !gimage.GetBuffer( cacheBuffer.data() ) )
    {
        spdlog::error( "loadSingle: cannot load data from file: {}", utf8string( path ) );
        return res;
    }
    size_t fulSize = size_t( data.dims.x )*data.dims.y*data.dims.z;
    if ( data.data.size() != fulSize )
        data.data.resize( fulSize );

    auto dimZ = dimsNum == 3 ? dims[2] : 1;
    auto dimXY = dims[0] * dims[1];
    auto dimXYZinv = dimZ * dimXY - dimXY;
    for ( unsigned z = 0; z < dimZ; ++z )
    {
        auto zOffset = z * dimXY;
        auto correctZOffset = needInvertZ ? ( dimXYZinv - zOffset ) : zOffset;
        for ( size_t i = 0; i < dimXY; ++i )
        {
            auto f = caster( &cacheBuffer[( correctZOffset + i ) * pixelSize] );
            res.min = std::min( res.min, f );
            res.max = std::max( res.max, f );
            data.data[zOffset + offset + i] = f;
        }
    }
    res.success = true;

    return res;
}

struct SeriesInfo
{
    float sliceSize{ 0.0f };
    int numSlices{ 0 };
    BitSet missedSlices;
};

SeriesInfo sortDICOMFiles( std::vector<std::filesystem::path>& files, unsigned maxNumThreads )
{
    SeriesInfo res;

    if ( files.empty() )
        return res;

    std::vector<SliceInfo> zOrder( files.size() );

    tbb::task_arena limitedArena( maxNumThreads );
    limitedArena.execute( [&]
    {
        tbb::parallel_for( tbb::blocked_range( 0, int( files.size() ) ),
            [&] ( const tbb::blocked_range<int>& range )
        {
            for ( int i = range.begin(); i < range.end(); ++i )
            {
                gdcm::ImageReader ir;
                std::ifstream ifs( files[i], std::ios_base::binary );
                ir.SetStream( ifs );
                ir.ReadSelectedTags( {
                    gdcm::Tag( 0x0002, 0x0002 ),
                    gdcm::Tag( 0x0008, 0x0016 ),
                    gdcm::Keywords::InstanceNumber::GetTag(),
                    gdcm::Keywords::ImagePositionPatient::GetTag() } );

                SliceInfo sl;
                sl.fileNum = i;
                const auto origin = gdcm::ImageHelper::GetOriginValue( ir.GetFile() );
                sl.z = origin[2];
                sl.imagePos = { origin[0], origin[1], origin[2] };

                // if Instance Number is available then sort by it
                const gdcm::DataSet& ds = ir.GetFile().GetDataSet();
                if( ds.FindDataElement( gdcm::Keywords::InstanceNumber::GetTag() ) )
                {
                    const gdcm::DataElement& de = ds.GetDataElement( gdcm::Keywords::InstanceNumber::GetTag() );
                    gdcm::Keywords::InstanceNumber at = {0}; // default value if empty
                    at.SetFromDataElement( de );
                    sl.instanceNum = at.GetValue();
                }
                zOrder[i] = sl;
            }
        } );
    } );

    bool zPosPresent = std::any_of( zOrder.begin(), zOrder.end(), [] ( const SliceInfo& el )
    {
        return el.z != 0.0;
    } );
    if ( !zPosPresent )
    {
        putScanFileNameInZ( files, zOrder );
    }

    sortScansByOrder( files, zOrder );

    if ( zOrder.size() > 1 )
    {
        auto denom = std::max( 1.0f, float( zOrder[1].instanceNum - zOrder[0].instanceNum ) );
        res.sliceSize = float( ( zOrder[1].imagePos - zOrder[0].imagePos ).length() / denom / 1000.0 );
        res.numSlices = zOrder.back().instanceNum - zOrder.front().instanceNum + 1;

        bool needReverse = zOrder[1].imagePos.z < zOrder[0].imagePos.z;

        if ( res.numSlices != 0 )
        {
            res.missedSlices.resize( res.numSlices );

            auto startIN = zOrder[0].instanceNum;
            for ( int i = 1; i < zOrder.size(); ++i )
            {
                auto prevIN = zOrder[i - 1].instanceNum;
                auto diff = zOrder[i].instanceNum - prevIN;
                if ( diff == 1 )
                    continue;
                if ( diff == 0 )
                {
                    res.numSlices = 0;
                    res.missedSlices.clear();
                    break; // non-consistent instances
                }

                int startMissedIndex = ( prevIN - startIN + 1 );
                for ( int j = startMissedIndex; j + 1 < startMissedIndex + diff; ++j )
                    res.missedSlices.set( needReverse ? ( res.numSlices - 1 - j ) : j );
            }
        }

        // if slices go in descending z-order then reverse them
        if ( needReverse )
            std::reverse( files.begin(), files.end() );
    }
    return res;
}

Expected<DicomVolume> loadSingleDicomFolder( std::vector<std::filesystem::path>& files,
                                                        unsigned maxNumThreads, const ProgressCallback& cb )
{
    MR_TIMER
    if ( !reportProgress( cb, 0.0f ) )
        return unexpected( "Loading canceled" );

    if ( files.empty() )
        return unexpected( "loadDCMFolder: there is no dcm file" );

    SimpleVolumeMinMax data;
    data.voxelSize = Vector3f();
    data.dims = Vector3i::diagonal( 0 );

    if ( files.size() == 1 )
        return loadDicomFile( files[0], subprogress( cb, 0.3f, 1.0f ) );
    auto seriesInfo = sortDICOMFiles( files, maxNumThreads );
    if ( seriesInfo.sliceSize != 0.0f )
        data.voxelSize.z = seriesInfo.sliceSize;

    if ( seriesInfo.numSlices == 0 )
        data.dims.z = ( int )files.size();
    else
        data.dims.z = seriesInfo.numSlices;

    auto firstRes = loadSingleFile( files.front(), data, 0 );
    if ( !firstRes.success )
        return unexpected( "loadDCMFolder: error loading first file \"" + utf8string( files.front() ) + "\"" );
    data.min = firstRes.min;
    data.max = firstRes.max;
    size_t dimXY = data.dims.x * data.dims.y;

    if ( !reportProgress( cb, 0.4f ) )
        return unexpected( "Loading canceled" );

    auto presentSlices = seriesInfo.missedSlices;
    presentSlices.resize( data.dims.z );
    presentSlices.flip();

    // other slices
    bool cancelCalled = false;
    std::vector<DCMFileLoadResult> slicesRes( files.size() - 1 );
    tbb::task_arena limitedArena( maxNumThreads );
    std::atomic<int> numLoadedSlices = 0;
    limitedArena.execute( [&]
    {
        cancelCalled = !ParallelFor( 0, int( slicesRes.size() ), [&] ( int i )
        {
            slicesRes[i] = loadSingleFile( files[i + 1], data, ( presentSlices.nthSetBit( i + 1 ) ) * dimXY );
            ++numLoadedSlices;
        }, subprogress( cb, 0.4f, 0.9f ), 1 );
    } );
    if ( cancelCalled )
        return unexpected( "Loading canceled" );

    // fill missed slices
    int missedSlicesNum = int( seriesInfo.missedSlices.count() );
    if ( missedSlicesNum != 0 )
    {
        int passedSlices = 0;
        int prevPresentSlice = -1;
        for ( auto presentSlice : presentSlices )
        {
            int numMissed = int( presentSlice ) - prevPresentSlice - 1;
            if ( numMissed == 0 )
            {
                prevPresentSlice = int( presentSlice );
                continue;
            }

            auto sb = subprogress( cb,
                0.9f + 0.1f * float( passedSlices ) / float( missedSlicesNum ),
                0.9f + 0.1f * float( passedSlices + numMissed ) / float( missedSlicesNum ) );
            int firstMissed = prevPresentSlice + 1;
            float ratioDenom = 1.0f / float( numMissed + 1 );
            cancelCalled = !ParallelFor( firstMissed * dimXY, presentSlice * dimXY, [&] ( size_t i )
            {
                auto posZ = int( i / dimXY );
                auto zBotDiff = posZ - prevPresentSlice;
                auto botValue = data.data[i - dimXY * zBotDiff];
                auto topValue = data.data[i + dimXY * ( int( presentSlice ) - posZ )];
                float ratio = float( zBotDiff ) * ratioDenom;
                data.data[i] = botValue * ( 1.0f - ratio ) + topValue * ratio;
            }, sb );
            if ( cancelCalled )
                break;
            prevPresentSlice = int( presentSlice );
            passedSlices += numMissed;
        }
    }

    if ( cancelCalled )
        return unexpected( "Loading canceled" );

    for ( const auto& sliceRes : slicesRes )
    {
        if ( !sliceRes.success )
            return {};
        data.min = std::min( sliceRes.min, data.min );
        data.max = std::max( sliceRes.max, data.max );
    }

    DicomVolume res;
    res.vol = std::move( data );
    if ( firstRes.seriesDescription.empty() )
         res.name = utf8string( files.front().parent_path().stem() );
    else
         res.name = firstRes.seriesDescription;
    res.xf = firstRes.xf;
    return res;
}

using SeriesMap = std::unordered_map<std::string, std::vector<std::filesystem::path>>;

Expected<SeriesMap,std::string> extractDCMSeries( const std::filesystem::path& path,
    const ProgressCallback& cb )
{
    std::error_code ec;
    if ( !std::filesystem::is_directory( path, ec ) )
        return { unexpected( "loadDCMFolder: path is not directory" ) };

    int filesNum = 0;
    std::vector<std::filesystem::path> files;
    for ( auto entry : Directory{ path, ec } )
    {
        if ( entry.is_regular_file( ec ) )
            ++filesNum;
    }

    std::unordered_map<std::string, std::vector<std::filesystem::path>> seriesMap;
    int fCounter = 0;
    for ( auto entry : Directory{ path, ec } )
    {
        ++fCounter;
        auto filePath = entry.path();
        std::string uid;
        if ( entry.is_regular_file( ec ) && isDicomFile( filePath, &uid ) )
            seriesMap[uid].push_back( filePath );
        if ( !reportProgress( cb, float( fCounter ) / float( filesNum ) ) )
            return { unexpected( "Loading canceled" ) };
    }

    if ( seriesMap.empty() )
        return unexpected( "No dcm series in folder: " + utf8string( path ) );

    return seriesMap;
}

std::vector<Expected<DicomVolume>> loadDicomsFolder( const std::filesystem::path& path,
                                                        unsigned maxNumThreads, const ProgressCallback& cb )
{
    auto seriesMap = extractDCMSeries( path, subprogress( cb, 0.0f, 0.3f ) );
    if ( !seriesMap.has_value() )
        return { unexpected( seriesMap.error() ) };

    int seriesCounter = 0;
    auto seriesNum = seriesMap->size();
    std::vector<Expected<DicomVolume>> res;
    for ( auto& [uid, series] : *seriesMap )
    {
        res.push_back( loadSingleDicomFolder( series, maxNumThreads,
            subprogress( cb,
                0.3f + 0.7f * float( seriesCounter ) / float( seriesNum ),
                0.3f + 0.7f * float( seriesCounter + 1 ) / float( seriesNum ) ) ) );

        ++seriesCounter;
        if ( !res.back().has_value() && res.back().error() == "Loading canceled" )
            return { unexpected( "Loading canceled" ) };
    }
    return res;
}

Expected<MR::VoxelsLoad::DicomVolume> loadDicomFolder( const std::filesystem::path& path, unsigned maxNumThreads /*= 4*/, const ProgressCallback& cb /*= {} */ )
{
    auto seriesMap = extractDCMSeries( path, subprogress( cb, 0.0f, 0.3f ) );
    if ( !seriesMap.has_value() )
        return { unexpected( seriesMap.error() ) };

    return loadSingleDicomFolder( seriesMap->begin()->second, maxNumThreads, subprogress( cb, 0.3f, 1.0f ) );
}

std::vector<Expected<LoadDCMResult>> loadDCMsFolder( const std::filesystem::path& path,
                                                     unsigned maxNumThreads, const ProgressCallback& cb )
{
    auto dicomRes = loadDicomsFolder( path, maxNumThreads, subprogress( cb, 0, 0.5f ) );
    std::vector<Expected<LoadDCMResult>> res( dicomRes.size() );
    for ( int i = 0; i < dicomRes.size(); ++i )
    {
        if ( !dicomRes[i].has_value() )
        {
            res[i] = unexpected( std::move( dicomRes[i].error() ) );
            continue;
        }
        res[i] = LoadDCMResult();
        res[i]->vdbVolume = simpleVolumeToVdbVolume( std::move( dicomRes[i]->vol ),
            subprogress( cb,
                0.5f + float( i ) / float( dicomRes.size() ) * 0.5f,
                0.5f + float( i + 1 ) / float( dicomRes.size() ) * 0.5f ) );
        res[i]->name = std::move( dicomRes[i]->name );
        res[i]->xf = std::move( dicomRes[i]->xf );
        if ( cb && !cb( 0.5f + float( i + 1 ) / float( dicomRes.size() ) * 0.5f ) )
            return { unexpected( "Loading canceled" ) };
    }

    return { res };
}

Expected<MR::VoxelsLoad::LoadDCMResult> loadDCMFolder( const std::filesystem::path& path, unsigned maxNumThreads /*= 4*/, const ProgressCallback& cb /*= {} */ )
{
    auto loadRes = loadDicomFolder( path, maxNumThreads, subprogress( cb, 0.0f, 0.5f ) );
    if ( !loadRes.has_value() )
        return unexpected( loadRes.error() );
    LoadDCMResult res;
    res.vdbVolume = simpleVolumeToVdbVolume( std::move( loadRes->vol ), subprogress( cb, 0.5f, 1.0f ) );
    res.name = std::move( loadRes->name );
    res.xf = std::move( loadRes->xf );
    return res;
}

std::vector<Expected<LoadDCMResult>> loadDCMFolderTree( const std::filesystem::path& path, unsigned maxNumThreads, const ProgressCallback& cb )
{
    MR_TIMER;
    std::vector<Expected<LoadDCMResult>> res;
    auto tryLoadDir = [&]( const std::filesystem::path& dir )
    {
        auto loadRes = loadDCMsFolder( dir, maxNumThreads, cb );
        if ( loadRes.size() == 1 && !loadRes[0].has_value() && loadRes[0].error() == "Loading canceled" )
            return false;

        res.insert( res.end(), std::make_move_iterator( loadRes.begin() ), std::make_move_iterator( loadRes.end() ) );

        return true;
    };
    if ( !tryLoadDir( path ) )
        return { unexpected( "Loading canceled" ) };

    std::error_code ec;
    for ( auto entry : DirectoryRecursive{ path, ec } )
    {
        if ( entry.is_directory( ec ) && !tryLoadDir( entry ) )
            break;
    }
    return res;
}

Expected<std::shared_ptr<ObjectVoxels>> createObjectVoxels( const LoadDCMResult & dcm, const ProgressCallback & cb )
{
    MR_TIMER
    std::shared_ptr<ObjectVoxels> obj = std::make_shared<ObjectVoxels>();
    obj->setName( dcm.name );
    obj->construct( dcm.vdbVolume );

    auto bins = obj->histogram().getBins();
    auto minMax = obj->histogram().getBinMinMax( bins.size() / 3 );
    auto isoRes = obj->setIsoValue( minMax.first, cb );
    if ( !isoRes )
        return unexpected( std::move( isoRes.error() ) );

    obj->select( true );
    obj->setXf( dcm.xf );
    return obj;
}

Expected<DicomVolume> loadDicomFile( const std::filesystem::path& path, const ProgressCallback& cb )
{
    MR_TIMER
    if ( !reportProgress( cb, 0.0f ) )
        return unexpected( "Loading canceled" );

    SimpleVolumeMinMax simpleVolume;
    simpleVolume.voxelSize = Vector3f();
    simpleVolume.dims.z = 1;
    auto fileRes = loadSingleFile( path, simpleVolume, 0 );
    if ( !fileRes.success )
        return unexpected( "loadDCMFile: error load file: " + utf8string( path ) );
    simpleVolume.max = fileRes.max;
    simpleVolume.min = fileRes.min;

    DicomVolume res;
    res.vol = std::move( simpleVolume );
    res.name = utf8string( path.stem() );
    return res;
}

} // namespace VoxelsLoad

namespace VoxelsSave
{

Expected<void> toDCM( const VdbVolume& vdbVolume, const std::filesystem::path& path, ProgressCallback cb )
{
    auto simpleVolume = vdbVolumeToSimpleVolumeU16( vdbVolume, {}, subprogress( cb, 0.f, 0.5f ) );
    if ( simpleVolume )
        return toDCM( *simpleVolume, path, subprogress( cb, 0.5f, 1.f ) );
    else
        return unexpected( simpleVolume.error() );
}

template <typename T>
Expected<void> toDCM( const VoxelsVolume<std::vector<T>>& volume, const std::filesystem::path& path, ProgressCallback cb )
{
    if ( !reportProgress( cb, 0.0f ) )
        return unexpected( "Loading canceled" );

    auto [gdcmScalar, gdcmTag] = getGDCMTypeAndTag<T>();

    gdcm::ImageWriter iw;
    auto& image = iw.GetImage();
    image.SetNumberOfDimensions( 3 );
    image.SetDimension( 0, volume.dims.x );
    image.SetDimension( 1, volume.dims.y );
    image.SetDimension( 2, volume.dims.z );
    image.SetPixelFormat( gdcm::PixelFormat( gdcmScalar ) );
    image.SetPhotometricInterpretation( gdcm::PhotometricInterpretation::MONOCHROME2 );
    image.SetSpacing( 0, volume.voxelSize.x * 1000.f );
    image.SetSpacing( 1, volume.voxelSize.y * 1000.f );
    image.SetSpacing( 2, volume.voxelSize.z * 1000.f );

    gdcm::DataElement data( gdcmTag );
    // copies full volume
    data.SetByteValue( reinterpret_cast<const char*>( volume.data.data() ), ( uint32_t )volume.data.size() * sizeof( T ) );
    if ( !reportProgress( cb, 0.5f ) )
        return unexpected( "Loading canceled" );
    image.SetDataElement( data );

    iw.SetImage( image );

    std::ofstream fout( path, std::ios_base::binary );
    iw.SetStream( fout );
    if ( !fout || !iw.Write() )
        return unexpected( "Cannot write DICOM file" );

    return {};
}

template Expected<void> toDCM<uint16_t>( const SimpleVolumeU16& volume, const std::filesystem::path& path, ProgressCallback cb );

MR_ON_INIT
{
    static const IOFilter filter( "Dicom (.dcm)", "*.dcm" );
    MR::VoxelsSave::setVoxelsSaver( filter, MR::VoxelsSave::toDCM );
    /* additionally register the general saver as an object saver for this format */
    MR::ObjectSave::setObjectSaver( filter, MR::saveObjectVoxelsToFile );
};

} // namespace VoxelsSave

} // namespace MR
#endif
