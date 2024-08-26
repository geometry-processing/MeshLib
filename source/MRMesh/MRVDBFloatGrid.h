#pragma once

#include "MRMeshFwd.h"
#include "MRVector3.h"

#ifndef MRMESH_NO_OPENVDB
// this header includes the whole OpenVDB, so please include it from .cpp files only

#include "MRPch/MROpenvdb.h"

namespace MR
{

/**
 * \defgroup BasicStructuresGroup Basic Structures
 * \brief This chapter represents documentation about basic structures elements
 * \{
 */

/// this class just hides very complex type of typedef openvdb::FloatGrid
struct OpenVdbFloatGrid : openvdb::FloatGrid
{
    OpenVdbFloatGrid() noexcept = default;
    OpenVdbFloatGrid( openvdb::FloatGrid && in ) : openvdb::FloatGrid( std::move( in ) ) {}
    [[nodiscard]] size_t heapBytes() const { return memUsage(); }
};

inline openvdb::FloatGrid & ovdb( OpenVdbFloatGrid & v ) { return v; }
inline const openvdb::FloatGrid & ovdb( const OpenVdbFloatGrid & v ) { return v; }

/// makes MR::FloatGrid shared pointer taking the contents of the input pointer
inline FloatGrid MakeFloatGrid( openvdb::FloatGrid::Ptr&& p )
{
    if ( !p )
        return {};
    return std::make_shared<OpenVdbFloatGrid>( std::move( *p ) );
}

inline Vector3i fromVdb( const openvdb::Coord & v )
{
    return Vector3i( v.x(), v.y(), v.z() );
}

inline openvdb::Coord toVdb( const Vector3i & v )
{
    return openvdb::Coord( v.x, v.y, v.z );
}

/// \}

}
#endif
