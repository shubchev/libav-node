#include "libav-node.h"


int main(int argc, char **argv) {
  auto enc = IAVEnc::create(1920, 1080, 30, EncQuality::MEDIUM);
  if (!enc) {
    return 1;
  }
   
  for (int i = 0; i < 900; i++) enc->encode(0, 0);

  return 0;
}