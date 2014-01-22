// Halide - to convert image space from RGB to YCbCr

// To compile
// g++ rgb2ycbcr.cpp -I ../include -L ../bin -lHalide -lpthread -ldl -lpng -o rgb2ycbcr
// LD_LIBRARY_PATH=../bin ./rgb2ycbcr

#include <Halide.h>
#include <stdio.h>
#include <algorithm>

#include "../test/performance/clock.h"

using namespace Halide;
using namespace std;

#include "../apps/support/image_io.h"

int main(int argc, char **argv){

	Var x("x"),y("y"),c("c");
	
	Image<uint8_t> rgbInput = load<uint8_t>("../apps/images/rgb.png");
	
	int width = rgbInput.width();
	int height = rgbInput.height();
	
//	width = 6; height = 6;
	
	int tile_extent_x = max(min(width/2,128),4);
	int tile_extent_y = max(min(height/2,128),1);
	
//	printf("The tile extents are %d (x) and %d (y).\n", tile_extent_x, tile_extent_y);
	
	Func T("T");
	T(x,y) = 0.0f;
	T(0,0) = 65.481f; T(1,0) = 128.533f; T(2,0) = 24.966f;
	T(0,1) = -37.797f; T(1,1) = -74.203f; T(2,1) = 112.0f;
	T(0,2) = 112.0f; T(1,2) = -93.786f; T(2,2) = -18.214f;
	
	T(x,y) = T(x,y)/255.0f;
	T.compute_root();
	
	Func offset("offset");
	offset(c) = 0;
	offset(0) = 16; offset(1) = 128; offset(2) = 128;
	offset.compute_root();
	
	Func yCbCr("yCbCr");
	yCbCr(x,y,c) = cast<uint8_t>(T(0,c)*rgbInput(x,y,0) + T(1,c)*rgbInput(x,y,1) + T(2,c)*rgbInput(x,y,2) + offset(c) + 0.5f); // additional 0.5 is to implement round() rather than floor() while type casting
	
	Var x_outer, x_inner, y_outer, y_inner, tile_index;
	yCbCr
		.tile( x, y, x_outer, y_outer, x_inner, y_inner, tile_extent_x, tile_extent_y)
		.fuse( x_outer, y_outer, tile_index)
		.parallel(tile_index);
		
	Var x_inner_outer, x_inner_inner; 
    yCbCr
        .split( x_inner, x_inner_outer, x_inner_inner, 4)
        .vectorize(x_inner_inner);
        
//	yCbCr.trace_stores();
//	offset.trace_stores();
//	T.trace_loads();

	Image<uint8_t> yCbCrOutput;
	
	double t1 = currentTime();
	int	numIters = 10;
	
	for(int iter=0; iter<numIters; iter++){
		yCbCrOutput = yCbCr.realize(width, height, rgbInput.channels());
	}
	
	double t2 = currentTime();
	printf("It took %f ms to complete\n",(t2-t1)/numIters);
	
	save(yCbCrOutput, "yCbCr_parrot.png");
	
	printf("Successfully converted.\n");
	return 0;

}
