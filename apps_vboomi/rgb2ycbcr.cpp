// Halide - to convert image space from RGB to YCbCr

// To compile
// g++ rgb2ycbcr.cpp -I ../include -L ../bin -lHalide -lpthread -ldl -lpng -o rgb2ycbcr
// LD_LIBRARY_PATH=../bin ./rgb2ycbcr

#include <Halide.h>
#include <stdio.h>

using namespace Halide;

#include "../apps/support/image_io.h"

int main(int argc, char **argv){

	Var x("x"),y("y"),c("c");
	
	Image<uint8_t> rgbInput = load<uint8_t>("../apps/images/rgb.png");
	
	Func T("T");
	T(x,y) = 0.0f;
	T(0,0) = 65.481f; T(1,0) = 128.533f; T(2,0) = 24.966f;
	T(0,1) = -37.797f; T(1,1) = -74.203f; T(2,1) = 112.0f;
	T(0,2) = 112.0f; T(1,2) = -93.786f; T(2,2) = -18.214f;
	
	T(x,y) = T(x,y)/255.0f;
//	T.compute_root();
	
	Func offset("offset");
	offset(c) = 0;
	offset(0) = 16; offset(1) = 128; offset(2) = 128;
	
	Func yCbCr("yCbCr");
	yCbCr(x,y,c) = cast<uint8_t>(T(0,c)*rgbInput(x,y,0) + T(1,c)*rgbInput(x,y,1) + T(2,c)*rgbInput(x,y,2) + offset(c) + 0.5f); // additional 0.5 is to implement round() rather than floor() while type casting
	
	yCbCr.trace_stores();
	T.trace_stores();
	T.trace_loads();
	
	Image<uint8_t> yCbCrOutput = yCbCr.realize(2,2,3); //rgbInput.width(), rgbInput.height(), rgbInput.channels());
	
	save(yCbCrOutput, "yCbCr_parrot.png");
	
	printf("Successfully converted.\n");
	return 0;

}
