// --------------------------------------------------------------
// SL 2018-01-02
// A simple, easily hackable CPU surface voxelizer
// MIT-license
// (c) Sylvain Lefebvre, https://github.com/sylefeb
// --------------------------------------------------------------

/*

Takes as input a file 'model.stl' from the source directory.
Outputs a voxel file named 'out.slab.vox' that can be imported
by 'MagicaVoxel' https://ephtracy.github.io/

Change VOXEL_RESOLUTION  to fit your needs.
Set    VOXEL_FILL_INSIDE to 1 to fill in the interior
Set    VOXEL_ROBUST_FILL to 1 to fill in the interior using
       a voting scheme (more robust, slower)

The basic principle is to rasterize triangles using three 2D axis
aligned grids, using integer arithmetic (fixed floating point)
for robust triangle interior checks.

Very simple and quite efficient despite a straightforward implementation.
Higher resolutions could easily be reached by not storing the
voxels as a 3D array of booleans (e.g. use blocking or an octree).

For the inside fill to work properly, the mesh has to be perfectly
watertight, with exactly matching vertices between neighboring
verticies.

*/

#include <LibSL/LibSL.h>

#include <iostream>
#include <algorithm>
#include <queue>
using namespace std;

#include "path.h"

// --------------------------------------------------------------

#define VOXEL_FILL_INSIDE 1
#define VOXEL_ROBUST_FILL 1

// --------------------------------------------------------------

#define FP_POW    16
#define FP_SCALE  (1<<FP_POW)

#define ALONG_X  1
#define ALONG_Y  2
#define ALONG_Z  4
#define INSIDE   8
#define INSIDE_X 16
#define INSIDE_Y 32
#define INSIDE_Z 64

// --------------------------------------------------------------

// saves a voxel file (.slab.vox format, can be imported by MagicaVoxel)
void saveAsVox(const char *fname, const Array3D<uchar>& voxs)
{
  Array<v3b> palette(256); // RGB palette
  palette.fill(0);
  palette[123] = v3b(127, 0, 127);
  palette[124] = v3b(255, 0, 0);
  palette[125] = v3b(0, 255, 0);
  palette[126] = v3b(0, 0, 255);
  palette[127] = v3b(255, 255, 255);
  FILE *f;
  f = fopen(fname, "wb");
  sl_assert(f != NULL);
  long sx = voxs.xsize(), sy = voxs.ysize(), sz = voxs.zsize();
  fwrite(&sx, 4, 1, f);
  fwrite(&sy, 4, 1, f);
  fwrite(&sz, 4, 1, f);
  ForRangeReverse(i, sx - 1, 0) {
    ForIndex(j, sy) {
      ForRangeReverse(k, sz - 1, 0) {
        uchar v   = voxs.at(i, j, k);
        uchar pal = v != 0 ? 127 : 255;
        if (v == INSIDE) {
          pal = 123;
        }
        fwrite(&pal, sizeof(uchar), 1, f);
      }
    }
  }
  fwrite(palette.raw(), sizeof(v3b), 256, f);
  fclose(f);
}

// saves a voxel file (.slab.vox format, can be imported by MagicaVoxel)
void saveAsBinvox(const char* fname, const Array3D<uchar>& voxs, unsigned int gridsize)
{
    ofstream output(fname, ios::out | ios::binary);
    sl_assert(output.good());
    long sx = voxs.xsize(), sy = voxs.ysize(), sz = voxs.zsize();
    // Write ASCII header
    output << "#binvox 1" << endl;
    output << "dim " << sx << " " << sy << " " << sz << "" << endl;
    output << "translate " << 0 << " " << 0 << " " << 0 << endl;
    output << "scale " << 1 << endl;
    output << "data" << endl;

    uchar currentvalue = voxs.at(0, 0, 0);
    uchar current_seen = 0;
    ForIndex(i, sx) {
        ForRangeReverse(j, sy - 1, 0) {
            ForIndex(k, sz) {
                uchar nextvalue = voxs.at(i, j, k);
                if (nextvalue != currentvalue || current_seen == (uchar)255) {
                    output.write((char*)&currentvalue, 1);
                    output.write((char*)&current_seen, 1);
                    currentvalue = nextvalue;
                    current_seen = 1;
                }
                else {
                    current_seen++;
                }
            }
        }
    }
    output.write((char*)&currentvalue, 1);
    output.write((char*)&current_seen, 1);
    output.close();
}

// --------------------------------------------------------------

inline bool isInTriangle(int i, int j, const v3i& p0, const v3i& p1, const v3i& p2, int& _depth)
{
  v2i delta_p0 = v2i(i, j) - v2i(p0);
  v2i delta_p1 = v2i(i, j) - v2i(p1);
  v2i delta_p2 = v2i(i, j) - v2i(p2);
  v2i delta10 = v2i(p1) - v2i(p0);
  v2i delta21 = v2i(p2) - v2i(p1);
  v2i delta02 = v2i(p0) - v2i(p2);

  int64_t c0 = (int64_t)delta_p0[0] * (int64_t)delta10[1] - (int64_t)delta_p0[1] * (int64_t)delta10[0];
  int64_t c1 = (int64_t)delta_p1[0] * (int64_t)delta21[1] - (int64_t)delta_p1[1] * (int64_t)delta21[0];
  int64_t c2 = (int64_t)delta_p2[0] * (int64_t)delta02[1] - (int64_t)delta_p2[1] * (int64_t)delta02[0];
  bool inside = (c0 <= 0 && c1 <= 0 && c2 <= 0) || (c0 >= 0 && c1 >= 0 && c2 >= 0);

  if (inside) {
    int64_t area = c0 + c1 + c2;
    int64_t b0 = (c1 << 10) / area;
    int64_t b1 = (c2 << 10) / area;
    int64_t b2 = (1 << 10) - b0 - b1;
    _depth = (int)((b0 * p0[2] + b1 * p1[2] + b2 * p2[2]) >> 10);
  }
  return inside;
}

// --------------------------------------------------------------

class swizzle_xyz
{
public:
  inline v3i forward(const v3i& v)  const { return v; }
  inline v3i backward(const v3i& v) const { return v; }
  inline int along() const { return ALONG_Z; }
};

class swizzle_zxy
{
public:
  inline v3i   forward(const v3i& v)  const { return v3i(v[2], v[0], v[1]); }
  inline v3i   backward(const v3i& v) const { return v3i(v[1], v[2], v[0]); }
  inline uchar along() const { return ALONG_Y; }
};

class swizzle_yzx
{
public:
  inline v3i   forward(const v3i& v)  const { return v3i(v[1], v[2], v[0]); }
  inline v3i   backward(const v3i& v) const { return v3i(v[2], v[0], v[1]); }
  inline uchar along() const { return ALONG_X; }
};

// --------------------------------------------------------------

template <class S>
void rasterize(
  const v3u&                  tri,
  const std::vector<v3i>&     pts,
  Array3D<uchar>&             _voxs)
{
  const S swizzler;
  v3i tripts[3] = {
    swizzler.forward(pts[tri[0]]),
    swizzler.forward(pts[tri[1]]),
    swizzler.forward(pts[tri[2]])
  };
  // check if triangle is valid
  v2i delta10 = v2i(tripts[1]) - v2i(tripts[0]);
  v2i delta21 = v2i(tripts[2]) - v2i(tripts[1]);
  v2i delta02 = v2i(tripts[0]) - v2i(tripts[2]);
  if (delta10 == v2i(0)) return;
  if (delta21 == v2i(0)) return;
  if (delta02 == v2i(0)) return;
  if (delta02[0] * delta10[1] - delta02[1] * delta10[0] == 0) return;
  // proceed
  AAB<2, int> pixbx;
  pixbx.addPoint(v2i(tripts[0]) / FP_SCALE);
  pixbx.addPoint(v2i(tripts[1]) / FP_SCALE);
  pixbx.addPoint(v2i(tripts[2]) / FP_SCALE);
  for (int j = pixbx.minCorner()[1]; j <= pixbx.maxCorner()[1]; j++) {
    for (int i = pixbx.minCorner()[0]; i <= pixbx.maxCorner()[0]; i++) {
      int depth;
      if (isInTriangle(
        (i << FP_POW) + (1 << (FP_POW - 1)), // centered
        (j << FP_POW) + (1 << (FP_POW - 1)), // centered
        tripts[0], tripts[1], tripts[2], depth)) {
        v3i vx = swizzler.backward(v3i(i, j, depth >> FP_POW));

        // TODO dirty hack; why are we even out of bounds?
        if (vx[0] < 0 || vx[0] >= _voxs.xsize()) continue;
        if (vx[1] < 0 || vx[1] >= _voxs.ysize()) continue;
        if (vx[2] < 0 || vx[2] >= _voxs.zsize()) continue;

        // tag the voxel as occupied
        // NOTE: voxels are likely to be hit multiple times (e.g. thin features)
        //       we flip the bit every time a hit occurs in a voxel
        _voxs.at(vx[0], vx[1], vx[2]) = ( _voxs.at(vx[0], vx[1], vx[2]) ^ swizzler.along() );
      }
    }
  }
}

// --------------------------------------------------------------

// This version is more robust by using all three direction
// and voting among them to decide what is filled or not
void fillInsideVoting(Array3D<uchar>& _voxs)
{
  // along x
  ForIndex(k, _voxs.zsize()) {
    ForIndex(j, _voxs.ysize()) {
      bool inside = false;
      ForIndex(i, _voxs.xsize()) {
        if (_voxs.at(i, j, k) & ALONG_X) {
          inside = !inside;
        }
        if (inside) {
          _voxs.at(i, j, k) |= INSIDE_X;
        }
      }
    }
  }
  // along y
  ForIndex(k, _voxs.zsize()) {
    ForIndex(j, _voxs.xsize()) {
      bool inside = false;
      ForIndex(i, _voxs.ysize()) {
        if (_voxs.at(j, i, k) & ALONG_Y) {
          inside = !inside;
        }
        if (inside) {
          _voxs.at(j, i, k) |= INSIDE_Y;
        }
      }
    }
  }
  // along z
  ForIndex(k, _voxs.ysize()) {
    ForIndex(j, _voxs.xsize()) {
      bool inside = false;
      ForIndex(i, _voxs.zsize()) {
        if (_voxs.at(j, k, i) & ALONG_Z) {
          inside = !inside;
        }
        if (inside) {
          _voxs.at(j, k, i) |= INSIDE_Z;
        }
      }
    }
  }
  // voting
  ForArray3D(_voxs, i, j, k) {
    uchar v = _voxs.at(i, j, k);
    int votes =
      (  (v & INSIDE_X) ? 1 : 0)
      + ((v & INSIDE_Y) ? 1 : 0)
      + ((v & INSIDE_Z) ? 1 : 0);
    // clean
    _voxs.at(i, j, k) &= ~(INSIDE_X | INSIDE_Y | INSIDE_Z);
    if (votes > 1) {
      // tag as inside
      _voxs.at(i, j, k) |= INSIDE;
    }
  }
}

// --------------------------------------------------------------

void fillInside(Array3D<uchar>& _voxs)
{
  ForIndex(k, _voxs.zsize()) {
    ForIndex(j, _voxs.ysize()) {
      bool inside = false;
      ForIndex(i, _voxs.xsize()) {
        if (_voxs.at(i, j, k) & ALONG_X) {
          inside = !inside;
        }
        if (inside) {
          _voxs.at(i, j, k) |= INSIDE;
        }
      }
    }
  }
}

// --------------------------------------------------------------

void printExample() {
    cout << "Example: cuda_voxelizer -f /home/jeroen/bunny.ply -s 512" << endl;
}

void printHelp() {
    fprintf(stdout, "\n## HELP  \n");
    cout << "Program options: " << endl << endl;
    cout << " -f <path to model file: .ply, .obj, .3ds> (required)" << endl;
    cout << " -s <voxelization grid size, power of 2: 8 -> 512, 1024, ... (default: 256)>" << endl;
    cout << " -o <output file name (default: derived from model file name & grid size)>" << endl;
    printExample();
    cout << endl;
}

inline bool file_exists(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

// Default options
string filename = "";
string filename_base = "";
string filename_output = "";

unsigned int gridsize = 256;

// Parse the program parameters and set them as global variables
void parseProgramParameters(int argc, char* argv[]) {
    if (argc < 2) { // not enough arguments
        fprintf(stdout, "Not enough program parameters. \n \n");
        printHelp();
        exit(0);
    }
    bool filegiven = false;
    for (int i = 1; i < argc; i++) {
        if (string(argv[i]) == "-f") {
            filename = argv[i + 1];
            filename_base = filename.substr(0, filename.find_last_of("."));
            filegiven = true;
            if (!file_exists(filename)) {
                fprintf(stdout, "[Err] File does not exist / cannot access: %s \n", filename.c_str());
                exit(1);
            }
            i++;
        }
        else if (string(argv[i]) == "-s") {
            gridsize = atoi(argv[i + 1]);
            i++;
        }
        else if (string(argv[i]) == "-h") {
            printHelp();
            exit(0);
        }
        else if (string(argv[i]) == "-o") {
            filename_output = argv[i + 1];
            i++;
        }
    }
    if (!filegiven) {
        fprintf(stdout, "[Err] You didn't specify a file using -f (path). This is required. Exiting. \n");
        printExample();
        exit(1);
    }

    if (filename_output == "") {
      //filename_output = filename_base + string("_") + to_string(gridsize) + string(".slab.vox");
      filename_output = filename_base + string("_") + to_string(gridsize) + string(".binvox");
    }

    fprintf(stdout, "[Info] Input Filename:  %s \n", filename.c_str());
    fprintf(stdout, "[Info] Output Filename: %s \n", filename_output.c_str());
    fprintf(stdout, "[Info] Grid size: %i \n", gridsize);
}


int main(int argc, char **argv)
{
  parseProgramParameters(argc, argv);

  try {

    // load triangle mesh
    TriangleMesh_Ptr mesh(loadTriangleMesh(filename.c_str()));
    // produce (fixed fp) integer vertices and triangles
    std::vector<v3i> pts;
    std::vector<v3u> tris;
    {
      float factor = 0.99f;
      m4x4f boxtrsf = scaleMatrix(v3f(gridsize * FP_SCALE))
        * scaleMatrix(v3f(1.f) / tupleMax(mesh->bbox().extent()))
        * translationMatrix((1 - factor) * 0.5f * mesh->bbox().extent())
        * scaleMatrix(v3f(factor))
        * translationMatrix(-mesh->bbox().minCorner());

      // transform vertices
      pts.resize(mesh->numVertices());
      ForIndex(p, mesh->numVertices()) {
        v3f pt   = mesh->posAt(p);
        v3f bxpt = boxtrsf.mulPoint(pt);
        v3i ipt  = v3i(clamp(round(bxpt), v3f(0.0f), v3f(gridsize * FP_SCALE) - v3f(1.0f)));
        pts[p]   = ipt;
      }
      // prepare triangles
      tris.reserve(mesh->numTriangles());
      ForIndex(t, mesh->numTriangles()) {
        v3u tri = mesh->triangleAt(t);
        tris.push_back(tri);
      }
    }

    // rasterize into voxels
    v3u resolution(mesh->bbox().extent() / tupleMax(mesh->bbox().extent()) * float(gridsize));
    Array3D<uchar> voxs(resolution);
    voxs.fill(0);
    {
      Timer tm("rasterization");
      Console::progressTextInit((int)tris.size());
      ForIndex(t, tris.size()) {
        Console::progressTextUpdate(t);
        rasterize<swizzle_xyz>(tris[t], pts, voxs); // xy view
        rasterize<swizzle_yzx>(tris[t], pts, voxs); // yz view
        rasterize<swizzle_zxy>(tris[t], pts, voxs); // zx view
      }
      Console::progressTextEnd();
      cerr << endl;
    }

    // add inner voxels
#if VOXEL_FILL_INSIDE
    {
      Timer tm("fill");
      cerr << "filling in/out ... ";
#if VOXEL_ROBUST_FILL
      fillInsideVoting(voxs);
#else
      fillInside(voxs);
#endif
      cerr << " done." << endl;
    }
#endif

    // save the result
    //saveAsVox(filename_output.c_str(), voxs);
    saveAsBinvox(filename_output.c_str(), voxs, gridsize);

    // report some stats
    int num_in_vox = 0;
    ForArray3D(voxs, i, j, k) {
      if (voxs.at(i, j, k) > 0) {
        num_in_vox++;
      }
    }
    cerr << "number of set voxels: " << num_in_vox << endl;

  } catch (Fatal& e) {
    cerr << "[ERROR] " << e.message() << endl;
  }

}

/* -------------------------------------------------------- */
