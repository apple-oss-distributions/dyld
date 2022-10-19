# Shared Cache Patch Table

The patch table contains a list of locations to modify when installing a root of a dylib.  It is also used when interposing, or updating weak values to point to a new definition.

## Patch Table V1

The version 1 patch table started as a field of the dyld3 shared cache closure.  It was later pulled out to its own entry in the shared cache header.

The V1 format was very space efficient.  Each used export in the cache contained a list of locations to patch.

The downside of this format was memory consumption when installing a root.  Instead of patching only locations in loaded dylibs, it patched all locations.

In dyld4, the dyld at runtime changed to only patch locations in used dylibs.  But this took more time to work out which locations to patch.

## Patch Table V2

The version 2 patch table was written to be fast at patching just used locations.  Specifically to work out "which locations do I need to patch when a root of a.dylib is installed and b.dylib is loaded?".

The patch stucture starts with a single header - dyld\_cache\_patch\_info\_v2.  That points to arrays for each of the different structures to support patching.  These are:

    dyld_cache_image_patches_v2        dylibs[];
    dyld_cache_image_export_v2         exports[];
    dyld_cache_image_clients_v2        clients[];
    dyld_cache_patchable_export_v2     clientUsedExports[];
    dyld_cache_patchable_location_v2   patchLocations[];
    const char*                        exportNames[];

### Dylib Exports

When a root is installed, the first step in dyld is to walk the used exports in that dylib, and see where they map to.  This can be done with:

    forEachExport(dylibIndex, callback)
        dyld_cache_image_patches_v2& dylib = dylibs[dylibIndex];
        dyld_cache_image_export_v2 dylibExports[] = exports.slice(dylib.exports)
        for ( auto& export : dylibExports )
            callback(export);

This list of exports is used later when inspecting each loaded dylib, but we only need to build this list once for the root dylib, and can re-use it with multiple clients.

Each of these exports is a name and location.  The name is an index in to the `exportNames` array, while the location is a 32-bit VM offset from the dylib being rooted.  An alternative would be a 64-bit cache offset, but that would take more space.
    
    struct dyld_cache_image_export_v2
    {
        uint32_t    dylibOffsetOfImpl;
        uint32_t    exportNameOffset;
    };

### Dylib Clients

For each installed root, we have to see if each loaded dylib is a client.  If so, the loaded clients must be patched.  To do so, we again start from the root dylib, and can get its clients with:

    forEachClient(dylibIndex, callback)
        dyld_cache_image_patches_v2& dylib = dylibs[dylibIndex];
        dyld_cache_image_clients_v2 dylibClients[] = clients.slice(dylib.clients)
        for ( auto& client : dylibClients )
            callback(client);

dyld can use the above to see if the client we are checking is in those returned from the callback.  Otherwise it can skip trying to patch that client for the given root.

### Locations to patch

Once a `(dylib, client)` pair is known to have locations to patch, we need to actually patch it.  The main operation here involves finding all of those locations.  Note the `dylibOffsetOfImpl` below is taken from the `dyld_cache_image_export_v2`, so the code below should be called on each export from the root dylib.

    forEachPatchableUseOfExportInImage(dylibIndex, dylibOffsetOfImpl, clientIndex, callback)
        dyld_cache_image_patches_v2& dylib = dylibs[dylibIndex];
        // Walk each client of this dylib, looking for the client we passed in
        for ( auto& client : clients.slice(dylib.clients) )
            if ( client.index != clientIndex ) continue;
            // Walk each export this client uses, looking for the export we passed in
            for ( auto& clientUsedExport : clientUsedExports.slice(client.usedExports) )
                if ( clientUsedExport.dylibOffsetOfImpl != dylibOffsetOfImpl ) continue;
                // Walk each use of this export in this client dylib
                for ( auto& patchLoc : patchLocations.slice(clientUsedExport.patches) )
                    callback(patchLoc);

## Patch Table V3

GOT uniquing breaks the `(dylib, client)` connection.  A single GOT does correspond to an export from `dylib`, but it may have an arbitrary number of clients.

This poses 2 main problems:

* The V2 patch table can't represent this `client`

* Patching GOTs may dirty locations which are not in use, as no clients of that GOT are loaded

I don't think the second point is a worry.  GOT uniquing should save so much memory overall, that dirtying a few unused locations shouldn't dramatically increase memory usage.  All GOTs for a given dylib should be adjacent, so its also likely some other GOT is used and the page is dirty.

The first point is a real problem though, and is why we need a V3 patch table.

### Differences between V2 and V3

V3 is an extension of V2.  All the same `(dylib, client)` data structures from V2 are still used.  The only difference is that there is an additional array for the GOT patches:

    dyld_cache_image_got_clients_v3 gotClients[];

A GOT client is very similar to a regular client struct, just that it lacks an index.  For example, here is the existing client struct:
    
    struct dyld_cache_image_clients_v2
    {
        uint32_t    clientDylibIndex;
        uint32_t    patchExportsStartIndex;
        uint32_t    patchExportsCount;
    };

and here is a GOT client:

    struct dyld_cache_image_got_clients_v3
    {
        uint32_t    patchExportsStartIndex;
        uint32_t    patchExportsCount;
    };

In both cases, they point to the `clientUsedExports` array.  The following finds all GOTs to patch for a given dylib export:

    forEachPatchableGOTUseOfExportInImage(dylibIndex, dylibOffsetOfImpl, callback)
        dyld_cache_image_patches_v2& dylib = dylibs[dylibIndex];
        dyld_cache_image_got_clients_v3& gots = gotClients[dylibIndex];
        // Walk each export this client uses, looking for the export we passed in
        for ( auto& clientUsedExport : clientUsedExports.slice(gots.usedExports) )
            if ( clientUsedExport.dylibOffsetOfImpl != dylibOffsetOfImpl ) continue;
            // Walk each use of this export in the GOT
            for ( auto& patchLoc : patchLocations.slice(clientUsedExport.patches) )
                callback(patchLoc);

Note that a single export may be used by multiple GOTs.  This is due to large caches where GOTs are combined within each cache file, but cannot be uniqued globally due to ISA constraints.  Also for arm64e, a single export may be used by GOTs where each use has a different key/diversification/etc.

## Stats

Note these are for V2.

Payload kind        | Total Size (KB)   | Number of instances   | Notes                 | 
:------------------:|:-----------------:|:---------------------:|:---------------------:|
Header              | 0.1               | 1                     |                       |
Dylibs              | 38                | 2362                  | Num cache dylibs      |
Dylib Exports       | 1261              | 157k                  | Total used exports    |
Dylib Clients       | 600               | 50k                   |                       |
Client Used Exports | 8544              | 712k                  |                       |
Patch Locations     | 22163             | 2.7m                  |                       |
Total               | 39986             |                       |                       |