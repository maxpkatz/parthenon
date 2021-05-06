#=========================================================================================
# (C) (or copyright) 2020. Triad National Security, LLC. All rights reserved.
#
# This program was produced under U.S. Government contract 89233218CNA000001 for Los
# Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
# for the U.S. Department of Energy/National Nuclear Security Administration. All rights
# in the program are reserved by Triad National Security, LLC, and the U.S. Department
# of Energy/National Nuclear Security Administration. The Government is granted for
# itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
# license in this material to reproduce, prepare derivative works, distribute copies to
# the public, perform publicly and display publicly, and to permit others to do so.
#=========================================================================================

from __future__ import print_function
#****************************************************************
# Note: reader import occurs after we fix the path at the bottom
#****************************************************************

#**************
# other imports
import os
import sys
import numpy as np
import argparse

def Usage():
    print("""

    Usage: %s [-quiet] [-brief] [-all] [-one] [--tol=eps] [-ignore_metadata] file1.phdf file2.phdf

                  -all: report all diffs at all positions
                  -one: Quit after first different variable
                -brief: Only report if files are different
                        Overrides --all
                -quiet: Only report if files are different and
                        don't print any extraneous info.
             --tol=eps: set tolerance to eps.  Default 1.0e-12
      -ignore_metadata: Ignore differences in metadata
             -relative: Compare relative differences using the
                        first file as the reference. Ignores
                        points where the first file is zero

    This example takes two hdf files and compares them to see if there are
    differences in the state variables.

    Shows how to load up a structure and prints one of the structures

"""%os.path.basename(__file__)
    )

def processArgs():
    parser = argparse.ArgumentParser(description="""
    arguments for differencing script
    """)
    parser.add_argument('-a', '-all', action='store_true', help='report all diffs at all positions')
    parser.add_argument('-t', '--tol', action='store', help='Sets tolerance for comparisons.  Default 1e-12')
    parser.add_argument('-o', '-one', action='store_true', help='Only report data for first different variable.')
    parser.add_argument('-b', '-brief', action='store_true', help='Only report if files are different.  Overrides -all')
    parser.add_argument('-q', '-quiet', action='store_true', help='Only report if files are different.  No other output. Overrides -all')
    parser.add_argument('-i', '-ignore_metadata', action='store_true', help='Ignore differences in metadata.')
    parser.add_argument('-r', '-relative', action='store_true', help='Compare relative differences.')
    parser.add_argument('files', nargs='*')

    return parser.parse_args()


def addPath():
    """ add the vis/python directory to the pythonpath variable """
    myPath = os.path.realpath(os.path.dirname(__file__))
    #sys.path.insert(0,myPath+'/../vis/python')
    #sys.path.insert(0,myPath+'/vis/python')

def compare_metadata( f0, f1, tol=1.0e-12):
    """ compares metadata of two hdf files f0 and f1. Returns 0 if the files are equivalent.

        Error codes:
            10 : Times in hdf files differ
            11 : Attribute names in Info of hdf files differ
            12 : Values of attributes in Info of hdf files differ
            13 : Attribute names in Params of hdf files differ
            14 : Values of attributes in Params of hdf files differ
            15 : Meta variables (Locations, VolumeLocations, LogicalLocations, Levels) differ
    """
    ERROR_TIME_DIFF=10
    ERROR_INFO_ATTRS_DIFF=11
    ERROR_INFO_VALUES_DIFF=12
    ERROR_PARAMS_ATTR_DIFF=13
    ERROR_PARAMS_VALUES_DIFF=14
    ERROR_META_VARS_DIFF=15


    #Compare the time in both files
    errTime = np.abs(f0.Time- f1.Time)
    if errTime > tol:
        print(f"""
        Time of outputs differ by {f0.Time - f1.Time}

        Quitting...
        """)
        return(ERROR_TIME_DIFF)

    #Compare the names of attributes in /Info, except "Time"
    f0_Info = { key:value for key,value in f0.Info.items() if key != "Time" and key != "BlocksPerPE" }
    f1_Info = { key:value for key,value in f1.Info.items() if key != "Time" and key != "BlocksPerPE" }
    if sorted(f0_Info.keys()) != sorted(f1_Info.keys()):
        print("""
        Names of attributes in '/Info' of differ

        Quitting...
        """)
        return(ERROR_INFO_ATTRS_DIFF)

    #Compare the values of attributes in /Info
    info_diffs = list( key for key in f0_Info.keys() if np.any(f0_Info[key] != f1_Info[key]))
    if len(info_diffs) > 0:
        print("\nValues of attributes in '/Info' differ\n")
        print("Differing attributes: ", info_diffs )
        print("\nQuitting...\n")
        return(ERROR_INFO_VALUES_DIFF)
    else:
        print('  %20s: no diffs'%"Info")

    f0_Params = f0.Params
    f1_Params = f1.Params
    #Comparing all params at once might work except for float point Params
    if sorted(f0_Params.keys()) != sorted(f1_Params.keys()):
        print("""
        Names of attributes in '/Params' of differ

        Quitting...
        """)
        return(ERROR_PARAMS_ATTRS_DIFF)

    #Check that the values of non-floats in Params match
    params_nonfloats_diffs = list(key for key in f0_Params.keys()
            if not isinstance(f0_Params[key], float) and f0_Params[key] != f1_Params[key] )


    #Check that the values of floats Params match
    params_floats_diffs = list(key for key in f0_Params.keys()
            if isinstance(f0_Params[key], float) and np.abs(f0_Params[key] - f1_Params[key]) > tol )

    if len(params_nonfloats_diffs) > 0 or len(params_floats_diffs) > 0:
        if len(params_nonfloats_diffs) > 0:
            print("\nValues of non-float attributes in '/Params' differ\n")
            print("Differing attributes: ",params_nonfloats_diffs  )
            print("\nQuitting...\n")
        elif len(params_floats_diffs) > 0:
            print("\nValues of float attributes in '/Params' differ\n")
            for key in params_floats_diffs:
                print(f"Param {key} differs by {f0_Params[key] - f1_Params[key]} ")
            print("\nQuitting...\n")
        return(ERROR_PARAMS_VALUES_DIFF)
    else:
        print('  %20s: no diffs'%"Params")

    # Now go through all variables in first file
    # and hunt for them in second file.
    #
    # Note that indices don't match when blocks
    # are different
    no_meta_variables_diff = True

    otherBlockIdx = list(f0.findBlockIdxInOther(f1,i) for i in range(f0.NumBlocks))

    for var in set(f0.Variables+f1.Variables):
        if var in ['Locations', 'VolumeLocations']:
            for key in f0.fid[var].keys():
                #Compare raw data of these variables
                val0 = f0.fid[var][key]
                val1 = f1.fid[var][key]

                #Sort val1 by otherBlockIdx
                val1 = val1[otherBlockIdx]

                # Compute norm error, check against tolerance
                err_val = np.abs(val0 - val1)
                err_mag = np.linalg.norm(err_val)
                if(err_mag > tol):
                    no_meta_variables_diff = False
                    if not quiet: print("")
                    print(f'Metavariable {var}/{key} differs between {f0.file} and {f1.file}')
                    if not quiet: print("")
                else:
                    print('  %18s/%s: no diffs'%(var,key))
        if var in ['LogicalLocations', 'Levels']:
            #Compare raw data of these variables
            val0 = np.array(f0.fid[var])
            val1 = np.array(f1.fid[var])

            #Sort val1 by otherBlockIdx
            val1 = val1[otherBlockIdx]

            #As integers, they should be identical
            if np.any(val0 != val1):
                no_meta_variables_diff = False
                if not quiet: print("")
                print(f'Metavariable {var} differs between {f0.file} and {f1.file}')
                if not quiet: print("")
            else:
                print('  %20s: no diffs'%var)

    if not no_meta_variables_diff:
        return(ERROR_META_VARS_DIFF)
    return(0)

def compare(files, all=False, brief=True, quiet=False, one=False, tol=1.0e-12, check_metadata=True, relative=False):
    """ compares two hdf files. Returns 0 if the files are equivalent.

        Error codes:
            1  : Can't open file 0
            2  : Can't open file 1
            3  : Total number of cells differ
            4  : Variable data in files differ

        Metadata Error codes:
            10 : Times in hdf files differ
            11 : Attribute names in Info of hdf files differ
            12 : Values of attributes in Info of hdf files differ
            13 : Attribute names in Params of hdf files differ
            14 : Values of attributes in Params of hdf files differ
            15 : Meta variables (Locations, VolumeLocations, LogicalLocations, Levels) differ
    """

    ERROR_NO_OPEN_F0 = 1
    ERROR_NO_OPEN_F1 = 2
    ERROR_CELLS_DIFFER = 3
    ERROR_DATA_DIFFER = 4

    #**************
    # import Reader
    #**************
    from phdf import phdf

    #**************
    # Reader Help
    #**************
    # for help  on phdf uncomment following line
    # print(help(phdf))


    # Load first file and print info
    f0 = phdf(files[0])
    try:
        f0 = phdf(files[0])
        if not quiet: print(f0)
    except:
        print("""
        *** ERROR: Unable to open %s as phdf file
        """%files[0])
        return(ERROR_NO_OPEN_F0)

    # Load second file and print info
    try:
        f1 = phdf(files[1])
        if not quiet:  print(f1)
    except:
        print("""
        *** ERROR: Unable to open %s as phdf file
        """%files[1])
        return(ERROR_NO_OPEN_F1)

    # rudimentary checks
    if f0.TotalCellsReal != f1.TotalCellsReal:
        # do both simulations have same number of cells?
        print("""
        These simulations have different number of cells.
        Clearly they are different.

        Quitting...
        """)
        return(ERROR_CELLS_DIFFER)

    if check_metadata:
        if not quiet: print("Checking metadata")
        metadata_status = compare_metadata(f0,f1)
        if( metadata_status != 0):
            return metadata_status
    else:
        if not quiet: print("Ignoring metadata")

    # Now go through all variables in first file
    # and hunt for them in second file.
    #
    # Note that indices don't match when blocks
    # are different
    no_diffs = True

    if not brief and not quiet:
        print('____Comparing on a per variable basis with tolerance %.16g'%tol)
    oneTenth = f0.TotalCells//10
    print('Tolerance = %g' % tol)

    #Make loc array of locations matching the shape of val0,val1
    #Useful for reporting locations of errors
    locations_x = f0.x
    locations_y = f0.y
    locations_z = f0.z

    #loc[dim,grid_idx,k,j,i]
    loc = np.empty((3,
                    locations_x.shape[0],
                    locations_z.shape[1],
                    locations_y.shape[1],
                    locations_x.shape[1]))

    #Share every coordinate 
    for grid_idx in range(loc.shape[1]):
        loc[:,grid_idx] = np.meshgrid(
                locations_z[grid_idx],
                locations_y[grid_idx],
                locations_x[grid_idx],
                indexing="ij")

    for var in set(f0.Variables+f1.Variables):
        var_no_diffs = True

        if var in ['Locations', 'VolumeLocations', 'LogicalLocations', 'Levels', 'Info', 'Params']:
            continue

        # Get values from file
        val0 = f0.Get(var,flatten=False)
        val1 = f1.Get(var,flatten=False)

        is_vec = np.prod(val0.shape) != f0.TotalCells

        #Determine arrangement of mesh blocks of f1 in terms of ordering in f0
        otherBlockIdx = list(f0.findBlockIdxInOther(f1,i) for i in range(f0.NumBlocks))

        #Rearrange val1 to match ordering of meshblocks in val0
        val1 = val1[otherBlockIdx]
        
        # compute error at every point
        if relative:
            err_val = np.abs((val0-val1)/val0)
            #Set error values where val0==0 to 0
            #Numpy masked arrays would be more robust here, but they are very slow
            err_val[val0==0] = 0
        else:
            err_val = np.abs(val0 - val1)

        # Compute magnitude of error at every point
        if is_vec:
            #Norm every vector
            err_mag = np.linalg.norm(err_val,axis=-1) 
        else:
            #Just plain error for scalars
            err_mag = err_mag
        err_max = err_mag.max()

        #Check if the error of any block exceeds the tolerance
        if err_max > tol:
            no_diffs = False
            var_no_diffs = False

            if quiet:
                continue #Skip reporting the error

            if one:
                #Print the maximum difference only
                bad_idx = np.argmax(err_mag)
                bad_idx = np.array(np.unravel_index(bad_idx,err_mag.shape))

                #Reshape for printing step
                bad_idxs = bad_idx.reshape((1,*bad_idx.shape))
            else:
                #Print all differences exceeding maximum
                bad_idxs = np.argwhere(err_mag > tol)

            for bad_idx in bad_idxs:
                bad_idx = tuple(bad_idx)

                #Find the bad location
                bad_loc = loc[:,bad_idx[0],bad_idx[1],bad_idx[2],bad_idx[3]]


                #TODO(forrestglines): Check that the bkji and zyx reported are the correct order
                print(f"Diff in {var:20s}")
                print(f"    bkji: ({bad_idx[0]:4d},{bad_idx[1]:4d},{bad_idx[2]:4d},{bad_idx[3]:4d})")
                print(f"    zyx: ({bad_loc[0]:4f},{bad_loc[1]:4f},{bad_loc[2]:4f})")
                print(f"    err_mag: {err_mag[bad_idx]:4f}")
                if is_vec:
                    print(f"    f0: " + " ".join(f"{u:.4e}" for u in val0[bad_idx]))
                    print(f"    f1: " + " ".join(f"{u:.4e}" for u in val1[bad_idx]))
                    print(f"    err: " + " ".join(f"{u:.4e}" for u in err_val[bad_idx]))
                else:
                    print(f"    f0: {val0[bad_idx]:.4e}")
                    print(f"    f1: {val1[bad_idx]:.4e}")
        if not quiet:
            if var_no_diffs:
                print(f"  {var:20s}: no diffs")
            else:
                print(f"  {var:20s}: differs")
    if no_diffs:
      return(0)
    else:
      return(ERROR_DATA_DIFFER)

if __name__ == "__main__":
    addPath()

    # process arguments
    input = processArgs()

    brief=input.b
    quiet=input.q
    one = input.o
    ignore_metadata = input.i
    relative = input.r

    check_metadata = not ignore_metadata

    # set all only if brief not set
    if brief or quiet:
        all=False
    else:
        all = input.a
    files = input.files

    if input.tol is not None:
        tol = float(input.tol)
    else:
        tol = 1.0e-12


    if len(files) != 2:
        Usage()
        sys.exit(1)

    ret = compare(files, all, brief, quiet, one, tol, check_metadata,relative)
    sys.exit(ret)