#pragma once

#include "MRVoxelsFwd.h"
#ifndef MRVOXELS_NO_DICOM
#include "MRVoxelsVolume.h"

#include "MRMesh/MRAffineXf.h"
#include "MRMesh/MRMatrix3.h"

#include <filesystem>

namespace MR
{

namespace VoxelsLoad
{

/// check if file is a valid DICOM dataset file
/// \param seriesUid - if set, the extracted series instance UID is copied to the variable
MRVOXELS_API bool isDicomFile( const std::filesystem::path& path, std::string* seriesUid = nullptr );

struct DicomVolume
{
    SimpleVolumeMinMax vol;
    std::string name;
    AffineXf3f xf;
};

struct LoadDCMResult
{
    VdbVolume vdbVolume;
    std::string name;
    AffineXf3f xf;
};

/// Loads 3D all volumetric data from DICOM files in a folder
MRVOXELS_API std::vector<Expected<LoadDCMResult>> loadDCMsFolder( const std::filesystem::path& path,
                                                                  unsigned maxNumThreads = 4, const ProgressCallback& cb = {} );
/// Loads 3D first volumetric data from DICOM files in a folder
MRVOXELS_API Expected<LoadDCMResult> loadDCMFolder( const std::filesystem::path& path,
                                                    unsigned maxNumThreads = 4, const ProgressCallback& cb = {} );

/// Loads 3D all volumetric data from DICOM files in a folder
MRVOXELS_API std::vector<Expected<DicomVolume>> loadDicomsFolder( const std::filesystem::path& path,
                                                                  unsigned maxNumThreads = 4, const ProgressCallback& cb = {} );
/// Loads 3D first volumetric data from DICOM files in a folder
MRVOXELS_API Expected<DicomVolume> loadDicomFolder( const std::filesystem::path& path,
                                                    unsigned maxNumThreads = 4, const ProgressCallback& cb = {} );

/// Loads every subfolder with DICOM volume as new object
MRVOXELS_API std::vector<Expected<LoadDCMResult>> loadDCMFolderTree( const std::filesystem::path& path,
                                                                     unsigned maxNumThreads = 4, const ProgressCallback& cb = {} );

/// converts LoadDCMResult in ObjectVoxels
MRVOXELS_API Expected<std::shared_ptr<ObjectVoxels>> createObjectVoxels( const LoadDCMResult & dcm, const ProgressCallback & cb = {} );

/// Loads 3D volumetric data from a single DICOM file
MRVOXELS_API Expected<DicomVolume> loadDicomFile( const std::filesystem::path& path, const ProgressCallback& cb = {} );

} // namespace VoxelsLoad

namespace VoxelsSave
{

/// Save voxels objet to a single 3d DICOM file
MRVOXELS_API Expected<void> toDCM( const VdbVolume& vdbVolume, const std::filesystem::path& path, ProgressCallback cb = {} );
template <typename T>
MRVOXELS_API Expected<void> toDCM( const VoxelsVolume<std::vector<T>>& volume, const std::filesystem::path& path, ProgressCallback cb = {} );

} // namespace VoxelsSave

} // namespace MR
#endif