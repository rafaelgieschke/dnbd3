/* crc32.c -- compute the CRC-32 of a data stream
 *
 * Modified for use in dnbd3
 * Original comment:
 *
 * Copyright (C) 1995-2006, 2010, 2011, 2012, 2016 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Thanks to Rodney Brown <rbrown64@csc.com.au> for his contribution of faster
 * CRC methods: exclusive-oring 32 bits of data at a time, and pre-computing
 * tables for updating the shift register in one step with three exclusive-ors
 * instead of four steps with four exclusive-ors.  This results in about a
 * factor of two increase in speed on a Power PC G4 (PPC7455) using gcc -O3.
 *
 * Original zlib.h license text:
 *

  Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu

*/

#include "../types.h"
#include <stddef.h>

#if defined(__x86_64__) || defined(__amd64__)
#include <emmintrin.h>
#include <smmintrin.h>
#include <wmmintrin.h>
#include <stdatomic.h>
#define zalign(n) __attribute__((aligned(n)))
#endif

#define OF(args) args

/* Definitions for doing the crc four data bytes at a time. */
#define TBLS 8

static const uint32_t crc_table[TBLS][256] =
{
  {
    0x00000000U, 0x77073096U, 0xee0e612cU, 0x990951baU, 0x076dc419U,
    0x706af48fU, 0xe963a535U, 0x9e6495a3U, 0x0edb8832U, 0x79dcb8a4U,
    0xe0d5e91eU, 0x97d2d988U, 0x09b64c2bU, 0x7eb17cbdU, 0xe7b82d07U,
    0x90bf1d91U, 0x1db71064U, 0x6ab020f2U, 0xf3b97148U, 0x84be41deU,
    0x1adad47dU, 0x6ddde4ebU, 0xf4d4b551U, 0x83d385c7U, 0x136c9856U,
    0x646ba8c0U, 0xfd62f97aU, 0x8a65c9ecU, 0x14015c4fU, 0x63066cd9U,
    0xfa0f3d63U, 0x8d080df5U, 0x3b6e20c8U, 0x4c69105eU, 0xd56041e4U,
    0xa2677172U, 0x3c03e4d1U, 0x4b04d447U, 0xd20d85fdU, 0xa50ab56bU,
    0x35b5a8faU, 0x42b2986cU, 0xdbbbc9d6U, 0xacbcf940U, 0x32d86ce3U,
    0x45df5c75U, 0xdcd60dcfU, 0xabd13d59U, 0x26d930acU, 0x51de003aU,
    0xc8d75180U, 0xbfd06116U, 0x21b4f4b5U, 0x56b3c423U, 0xcfba9599U,
    0xb8bda50fU, 0x2802b89eU, 0x5f058808U, 0xc60cd9b2U, 0xb10be924U,
    0x2f6f7c87U, 0x58684c11U, 0xc1611dabU, 0xb6662d3dU, 0x76dc4190U,
    0x01db7106U, 0x98d220bcU, 0xefd5102aU, 0x71b18589U, 0x06b6b51fU,
    0x9fbfe4a5U, 0xe8b8d433U, 0x7807c9a2U, 0x0f00f934U, 0x9609a88eU,
    0xe10e9818U, 0x7f6a0dbbU, 0x086d3d2dU, 0x91646c97U, 0xe6635c01U,
    0x6b6b51f4U, 0x1c6c6162U, 0x856530d8U, 0xf262004eU, 0x6c0695edU,
    0x1b01a57bU, 0x8208f4c1U, 0xf50fc457U, 0x65b0d9c6U, 0x12b7e950U,
    0x8bbeb8eaU, 0xfcb9887cU, 0x62dd1ddfU, 0x15da2d49U, 0x8cd37cf3U,
    0xfbd44c65U, 0x4db26158U, 0x3ab551ceU, 0xa3bc0074U, 0xd4bb30e2U,
    0x4adfa541U, 0x3dd895d7U, 0xa4d1c46dU, 0xd3d6f4fbU, 0x4369e96aU,
    0x346ed9fcU, 0xad678846U, 0xda60b8d0U, 0x44042d73U, 0x33031de5U,
    0xaa0a4c5fU, 0xdd0d7cc9U, 0x5005713cU, 0x270241aaU, 0xbe0b1010U,
    0xc90c2086U, 0x5768b525U, 0x206f85b3U, 0xb966d409U, 0xce61e49fU,
    0x5edef90eU, 0x29d9c998U, 0xb0d09822U, 0xc7d7a8b4U, 0x59b33d17U,
    0x2eb40d81U, 0xb7bd5c3bU, 0xc0ba6cadU, 0xedb88320U, 0x9abfb3b6U,
    0x03b6e20cU, 0x74b1d29aU, 0xead54739U, 0x9dd277afU, 0x04db2615U,
    0x73dc1683U, 0xe3630b12U, 0x94643b84U, 0x0d6d6a3eU, 0x7a6a5aa8U,
    0xe40ecf0bU, 0x9309ff9dU, 0x0a00ae27U, 0x7d079eb1U, 0xf00f9344U,
    0x8708a3d2U, 0x1e01f268U, 0x6906c2feU, 0xf762575dU, 0x806567cbU,
    0x196c3671U, 0x6e6b06e7U, 0xfed41b76U, 0x89d32be0U, 0x10da7a5aU,
    0x67dd4accU, 0xf9b9df6fU, 0x8ebeeff9U, 0x17b7be43U, 0x60b08ed5U,
    0xd6d6a3e8U, 0xa1d1937eU, 0x38d8c2c4U, 0x4fdff252U, 0xd1bb67f1U,
    0xa6bc5767U, 0x3fb506ddU, 0x48b2364bU, 0xd80d2bdaU, 0xaf0a1b4cU,
    0x36034af6U, 0x41047a60U, 0xdf60efc3U, 0xa867df55U, 0x316e8eefU,
    0x4669be79U, 0xcb61b38cU, 0xbc66831aU, 0x256fd2a0U, 0x5268e236U,
    0xcc0c7795U, 0xbb0b4703U, 0x220216b9U, 0x5505262fU, 0xc5ba3bbeU,
    0xb2bd0b28U, 0x2bb45a92U, 0x5cb36a04U, 0xc2d7ffa7U, 0xb5d0cf31U,
    0x2cd99e8bU, 0x5bdeae1dU, 0x9b64c2b0U, 0xec63f226U, 0x756aa39cU,
    0x026d930aU, 0x9c0906a9U, 0xeb0e363fU, 0x72076785U, 0x05005713U,
    0x95bf4a82U, 0xe2b87a14U, 0x7bb12baeU, 0x0cb61b38U, 0x92d28e9bU,
    0xe5d5be0dU, 0x7cdcefb7U, 0x0bdbdf21U, 0x86d3d2d4U, 0xf1d4e242U,
    0x68ddb3f8U, 0x1fda836eU, 0x81be16cdU, 0xf6b9265bU, 0x6fb077e1U,
    0x18b74777U, 0x88085ae6U, 0xff0f6a70U, 0x66063bcaU, 0x11010b5cU,
    0x8f659effU, 0xf862ae69U, 0x616bffd3U, 0x166ccf45U, 0xa00ae278U,
    0xd70dd2eeU, 0x4e048354U, 0x3903b3c2U, 0xa7672661U, 0xd06016f7U,
    0x4969474dU, 0x3e6e77dbU, 0xaed16a4aU, 0xd9d65adcU, 0x40df0b66U,
    0x37d83bf0U, 0xa9bcae53U, 0xdebb9ec5U, 0x47b2cf7fU, 0x30b5ffe9U,
    0xbdbdf21cU, 0xcabac28aU, 0x53b39330U, 0x24b4a3a6U, 0xbad03605U,
    0xcdd70693U, 0x54de5729U, 0x23d967bfU, 0xb3667a2eU, 0xc4614ab8U,
    0x5d681b02U, 0x2a6f2b94U, 0xb40bbe37U, 0xc30c8ea1U, 0x5a05df1bU,
    0x2d02ef8dU
  },
  {
    0x00000000U, 0x191b3141U, 0x32366282U, 0x2b2d53c3U, 0x646cc504U,
    0x7d77f445U, 0x565aa786U, 0x4f4196c7U, 0xc8d98a08U, 0xd1c2bb49U,
    0xfaefe88aU, 0xe3f4d9cbU, 0xacb54f0cU, 0xb5ae7e4dU, 0x9e832d8eU,
    0x87981ccfU, 0x4ac21251U, 0x53d92310U, 0x78f470d3U, 0x61ef4192U,
    0x2eaed755U, 0x37b5e614U, 0x1c98b5d7U, 0x05838496U, 0x821b9859U,
    0x9b00a918U, 0xb02dfadbU, 0xa936cb9aU, 0xe6775d5dU, 0xff6c6c1cU,
    0xd4413fdfU, 0xcd5a0e9eU, 0x958424a2U, 0x8c9f15e3U, 0xa7b24620U,
    0xbea97761U, 0xf1e8e1a6U, 0xe8f3d0e7U, 0xc3de8324U, 0xdac5b265U,
    0x5d5daeaaU, 0x44469febU, 0x6f6bcc28U, 0x7670fd69U, 0x39316baeU,
    0x202a5aefU, 0x0b07092cU, 0x121c386dU, 0xdf4636f3U, 0xc65d07b2U,
    0xed705471U, 0xf46b6530U, 0xbb2af3f7U, 0xa231c2b6U, 0x891c9175U,
    0x9007a034U, 0x179fbcfbU, 0x0e848dbaU, 0x25a9de79U, 0x3cb2ef38U,
    0x73f379ffU, 0x6ae848beU, 0x41c51b7dU, 0x58de2a3cU, 0xf0794f05U,
    0xe9627e44U, 0xc24f2d87U, 0xdb541cc6U, 0x94158a01U, 0x8d0ebb40U,
    0xa623e883U, 0xbf38d9c2U, 0x38a0c50dU, 0x21bbf44cU, 0x0a96a78fU,
    0x138d96ceU, 0x5ccc0009U, 0x45d73148U, 0x6efa628bU, 0x77e153caU,
    0xbabb5d54U, 0xa3a06c15U, 0x888d3fd6U, 0x91960e97U, 0xded79850U,
    0xc7cca911U, 0xece1fad2U, 0xf5facb93U, 0x7262d75cU, 0x6b79e61dU,
    0x4054b5deU, 0x594f849fU, 0x160e1258U, 0x0f152319U, 0x243870daU,
    0x3d23419bU, 0x65fd6ba7U, 0x7ce65ae6U, 0x57cb0925U, 0x4ed03864U,
    0x0191aea3U, 0x188a9fe2U, 0x33a7cc21U, 0x2abcfd60U, 0xad24e1afU,
    0xb43fd0eeU, 0x9f12832dU, 0x8609b26cU, 0xc94824abU, 0xd05315eaU,
    0xfb7e4629U, 0xe2657768U, 0x2f3f79f6U, 0x362448b7U, 0x1d091b74U,
    0x04122a35U, 0x4b53bcf2U, 0x52488db3U, 0x7965de70U, 0x607eef31U,
    0xe7e6f3feU, 0xfefdc2bfU, 0xd5d0917cU, 0xcccba03dU, 0x838a36faU,
    0x9a9107bbU, 0xb1bc5478U, 0xa8a76539U, 0x3b83984bU, 0x2298a90aU,
    0x09b5fac9U, 0x10aecb88U, 0x5fef5d4fU, 0x46f46c0eU, 0x6dd93fcdU,
    0x74c20e8cU, 0xf35a1243U, 0xea412302U, 0xc16c70c1U, 0xd8774180U,
    0x9736d747U, 0x8e2de606U, 0xa500b5c5U, 0xbc1b8484U, 0x71418a1aU,
    0x685abb5bU, 0x4377e898U, 0x5a6cd9d9U, 0x152d4f1eU, 0x0c367e5fU,
    0x271b2d9cU, 0x3e001cddU, 0xb9980012U, 0xa0833153U, 0x8bae6290U,
    0x92b553d1U, 0xddf4c516U, 0xc4eff457U, 0xefc2a794U, 0xf6d996d5U,
    0xae07bce9U, 0xb71c8da8U, 0x9c31de6bU, 0x852aef2aU, 0xca6b79edU,
    0xd37048acU, 0xf85d1b6fU, 0xe1462a2eU, 0x66de36e1U, 0x7fc507a0U,
    0x54e85463U, 0x4df36522U, 0x02b2f3e5U, 0x1ba9c2a4U, 0x30849167U,
    0x299fa026U, 0xe4c5aeb8U, 0xfdde9ff9U, 0xd6f3cc3aU, 0xcfe8fd7bU,
    0x80a96bbcU, 0x99b25afdU, 0xb29f093eU, 0xab84387fU, 0x2c1c24b0U,
    0x350715f1U, 0x1e2a4632U, 0x07317773U, 0x4870e1b4U, 0x516bd0f5U,
    0x7a468336U, 0x635db277U, 0xcbfad74eU, 0xd2e1e60fU, 0xf9ccb5ccU,
    0xe0d7848dU, 0xaf96124aU, 0xb68d230bU, 0x9da070c8U, 0x84bb4189U,
    0x03235d46U, 0x1a386c07U, 0x31153fc4U, 0x280e0e85U, 0x674f9842U,
    0x7e54a903U, 0x5579fac0U, 0x4c62cb81U, 0x8138c51fU, 0x9823f45eU,
    0xb30ea79dU, 0xaa1596dcU, 0xe554001bU, 0xfc4f315aU, 0xd7626299U,
    0xce7953d8U, 0x49e14f17U, 0x50fa7e56U, 0x7bd72d95U, 0x62cc1cd4U,
    0x2d8d8a13U, 0x3496bb52U, 0x1fbbe891U, 0x06a0d9d0U, 0x5e7ef3ecU,
    0x4765c2adU, 0x6c48916eU, 0x7553a02fU, 0x3a1236e8U, 0x230907a9U,
    0x0824546aU, 0x113f652bU, 0x96a779e4U, 0x8fbc48a5U, 0xa4911b66U,
    0xbd8a2a27U, 0xf2cbbce0U, 0xebd08da1U, 0xc0fdde62U, 0xd9e6ef23U,
    0x14bce1bdU, 0x0da7d0fcU, 0x268a833fU, 0x3f91b27eU, 0x70d024b9U,
    0x69cb15f8U, 0x42e6463bU, 0x5bfd777aU, 0xdc656bb5U, 0xc57e5af4U,
    0xee530937U, 0xf7483876U, 0xb809aeb1U, 0xa1129ff0U, 0x8a3fcc33U,
    0x9324fd72U
  },
  {
    0x00000000U, 0x01c26a37U, 0x0384d46eU, 0x0246be59U, 0x0709a8dcU,
    0x06cbc2ebU, 0x048d7cb2U, 0x054f1685U, 0x0e1351b8U, 0x0fd13b8fU,
    0x0d9785d6U, 0x0c55efe1U, 0x091af964U, 0x08d89353U, 0x0a9e2d0aU,
    0x0b5c473dU, 0x1c26a370U, 0x1de4c947U, 0x1fa2771eU, 0x1e601d29U,
    0x1b2f0bacU, 0x1aed619bU, 0x18abdfc2U, 0x1969b5f5U, 0x1235f2c8U,
    0x13f798ffU, 0x11b126a6U, 0x10734c91U, 0x153c5a14U, 0x14fe3023U,
    0x16b88e7aU, 0x177ae44dU, 0x384d46e0U, 0x398f2cd7U, 0x3bc9928eU,
    0x3a0bf8b9U, 0x3f44ee3cU, 0x3e86840bU, 0x3cc03a52U, 0x3d025065U,
    0x365e1758U, 0x379c7d6fU, 0x35dac336U, 0x3418a901U, 0x3157bf84U,
    0x3095d5b3U, 0x32d36beaU, 0x331101ddU, 0x246be590U, 0x25a98fa7U,
    0x27ef31feU, 0x262d5bc9U, 0x23624d4cU, 0x22a0277bU, 0x20e69922U,
    0x2124f315U, 0x2a78b428U, 0x2bbade1fU, 0x29fc6046U, 0x283e0a71U,
    0x2d711cf4U, 0x2cb376c3U, 0x2ef5c89aU, 0x2f37a2adU, 0x709a8dc0U,
    0x7158e7f7U, 0x731e59aeU, 0x72dc3399U, 0x7793251cU, 0x76514f2bU,
    0x7417f172U, 0x75d59b45U, 0x7e89dc78U, 0x7f4bb64fU, 0x7d0d0816U,
    0x7ccf6221U, 0x798074a4U, 0x78421e93U, 0x7a04a0caU, 0x7bc6cafdU,
    0x6cbc2eb0U, 0x6d7e4487U, 0x6f38fadeU, 0x6efa90e9U, 0x6bb5866cU,
    0x6a77ec5bU, 0x68315202U, 0x69f33835U, 0x62af7f08U, 0x636d153fU,
    0x612bab66U, 0x60e9c151U, 0x65a6d7d4U, 0x6464bde3U, 0x662203baU,
    0x67e0698dU, 0x48d7cb20U, 0x4915a117U, 0x4b531f4eU, 0x4a917579U,
    0x4fde63fcU, 0x4e1c09cbU, 0x4c5ab792U, 0x4d98dda5U, 0x46c49a98U,
    0x4706f0afU, 0x45404ef6U, 0x448224c1U, 0x41cd3244U, 0x400f5873U,
    0x4249e62aU, 0x438b8c1dU, 0x54f16850U, 0x55330267U, 0x5775bc3eU,
    0x56b7d609U, 0x53f8c08cU, 0x523aaabbU, 0x507c14e2U, 0x51be7ed5U,
    0x5ae239e8U, 0x5b2053dfU, 0x5966ed86U, 0x58a487b1U, 0x5deb9134U,
    0x5c29fb03U, 0x5e6f455aU, 0x5fad2f6dU, 0xe1351b80U, 0xe0f771b7U,
    0xe2b1cfeeU, 0xe373a5d9U, 0xe63cb35cU, 0xe7fed96bU, 0xe5b86732U,
    0xe47a0d05U, 0xef264a38U, 0xeee4200fU, 0xeca29e56U, 0xed60f461U,
    0xe82fe2e4U, 0xe9ed88d3U, 0xebab368aU, 0xea695cbdU, 0xfd13b8f0U,
    0xfcd1d2c7U, 0xfe976c9eU, 0xff5506a9U, 0xfa1a102cU, 0xfbd87a1bU,
    0xf99ec442U, 0xf85cae75U, 0xf300e948U, 0xf2c2837fU, 0xf0843d26U,
    0xf1465711U, 0xf4094194U, 0xf5cb2ba3U, 0xf78d95faU, 0xf64fffcdU,
    0xd9785d60U, 0xd8ba3757U, 0xdafc890eU, 0xdb3ee339U, 0xde71f5bcU,
    0xdfb39f8bU, 0xddf521d2U, 0xdc374be5U, 0xd76b0cd8U, 0xd6a966efU,
    0xd4efd8b6U, 0xd52db281U, 0xd062a404U, 0xd1a0ce33U, 0xd3e6706aU,
    0xd2241a5dU, 0xc55efe10U, 0xc49c9427U, 0xc6da2a7eU, 0xc7184049U,
    0xc25756ccU, 0xc3953cfbU, 0xc1d382a2U, 0xc011e895U, 0xcb4dafa8U,
    0xca8fc59fU, 0xc8c97bc6U, 0xc90b11f1U, 0xcc440774U, 0xcd866d43U,
    0xcfc0d31aU, 0xce02b92dU, 0x91af9640U, 0x906dfc77U, 0x922b422eU,
    0x93e92819U, 0x96a63e9cU, 0x976454abU, 0x9522eaf2U, 0x94e080c5U,
    0x9fbcc7f8U, 0x9e7eadcfU, 0x9c381396U, 0x9dfa79a1U, 0x98b56f24U,
    0x99770513U, 0x9b31bb4aU, 0x9af3d17dU, 0x8d893530U, 0x8c4b5f07U,
    0x8e0de15eU, 0x8fcf8b69U, 0x8a809decU, 0x8b42f7dbU, 0x89044982U,
    0x88c623b5U, 0x839a6488U, 0x82580ebfU, 0x801eb0e6U, 0x81dcdad1U,
    0x8493cc54U, 0x8551a663U, 0x8717183aU, 0x86d5720dU, 0xa9e2d0a0U,
    0xa820ba97U, 0xaa6604ceU, 0xaba46ef9U, 0xaeeb787cU, 0xaf29124bU,
    0xad6fac12U, 0xacadc625U, 0xa7f18118U, 0xa633eb2fU, 0xa4755576U,
    0xa5b73f41U, 0xa0f829c4U, 0xa13a43f3U, 0xa37cfdaaU, 0xa2be979dU,
    0xb5c473d0U, 0xb40619e7U, 0xb640a7beU, 0xb782cd89U, 0xb2cddb0cU,
    0xb30fb13bU, 0xb1490f62U, 0xb08b6555U, 0xbbd72268U, 0xba15485fU,
    0xb853f606U, 0xb9919c31U, 0xbcde8ab4U, 0xbd1ce083U, 0xbf5a5edaU,
    0xbe9834edU
  },
  {
    0x00000000U, 0xb8bc6765U, 0xaa09c88bU, 0x12b5afeeU, 0x8f629757U,
    0x37def032U, 0x256b5fdcU, 0x9dd738b9U, 0xc5b428efU, 0x7d084f8aU,
    0x6fbde064U, 0xd7018701U, 0x4ad6bfb8U, 0xf26ad8ddU, 0xe0df7733U,
    0x58631056U, 0x5019579fU, 0xe8a530faU, 0xfa109f14U, 0x42acf871U,
    0xdf7bc0c8U, 0x67c7a7adU, 0x75720843U, 0xcdce6f26U, 0x95ad7f70U,
    0x2d111815U, 0x3fa4b7fbU, 0x8718d09eU, 0x1acfe827U, 0xa2738f42U,
    0xb0c620acU, 0x087a47c9U, 0xa032af3eU, 0x188ec85bU, 0x0a3b67b5U,
    0xb28700d0U, 0x2f503869U, 0x97ec5f0cU, 0x8559f0e2U, 0x3de59787U,
    0x658687d1U, 0xdd3ae0b4U, 0xcf8f4f5aU, 0x7733283fU, 0xeae41086U,
    0x525877e3U, 0x40edd80dU, 0xf851bf68U, 0xf02bf8a1U, 0x48979fc4U,
    0x5a22302aU, 0xe29e574fU, 0x7f496ff6U, 0xc7f50893U, 0xd540a77dU,
    0x6dfcc018U, 0x359fd04eU, 0x8d23b72bU, 0x9f9618c5U, 0x272a7fa0U,
    0xbafd4719U, 0x0241207cU, 0x10f48f92U, 0xa848e8f7U, 0x9b14583dU,
    0x23a83f58U, 0x311d90b6U, 0x89a1f7d3U, 0x1476cf6aU, 0xaccaa80fU,
    0xbe7f07e1U, 0x06c36084U, 0x5ea070d2U, 0xe61c17b7U, 0xf4a9b859U,
    0x4c15df3cU, 0xd1c2e785U, 0x697e80e0U, 0x7bcb2f0eU, 0xc377486bU,
    0xcb0d0fa2U, 0x73b168c7U, 0x6104c729U, 0xd9b8a04cU, 0x446f98f5U,
    0xfcd3ff90U, 0xee66507eU, 0x56da371bU, 0x0eb9274dU, 0xb6054028U,
    0xa4b0efc6U, 0x1c0c88a3U, 0x81dbb01aU, 0x3967d77fU, 0x2bd27891U,
    0x936e1ff4U, 0x3b26f703U, 0x839a9066U, 0x912f3f88U, 0x299358edU,
    0xb4446054U, 0x0cf80731U, 0x1e4da8dfU, 0xa6f1cfbaU, 0xfe92dfecU,
    0x462eb889U, 0x549b1767U, 0xec277002U, 0x71f048bbU, 0xc94c2fdeU,
    0xdbf98030U, 0x6345e755U, 0x6b3fa09cU, 0xd383c7f9U, 0xc1366817U,
    0x798a0f72U, 0xe45d37cbU, 0x5ce150aeU, 0x4e54ff40U, 0xf6e89825U,
    0xae8b8873U, 0x1637ef16U, 0x048240f8U, 0xbc3e279dU, 0x21e91f24U,
    0x99557841U, 0x8be0d7afU, 0x335cb0caU, 0xed59b63bU, 0x55e5d15eU,
    0x47507eb0U, 0xffec19d5U, 0x623b216cU, 0xda874609U, 0xc832e9e7U,
    0x708e8e82U, 0x28ed9ed4U, 0x9051f9b1U, 0x82e4565fU, 0x3a58313aU,
    0xa78f0983U, 0x1f336ee6U, 0x0d86c108U, 0xb53aa66dU, 0xbd40e1a4U,
    0x05fc86c1U, 0x1749292fU, 0xaff54e4aU, 0x322276f3U, 0x8a9e1196U,
    0x982bbe78U, 0x2097d91dU, 0x78f4c94bU, 0xc048ae2eU, 0xd2fd01c0U,
    0x6a4166a5U, 0xf7965e1cU, 0x4f2a3979U, 0x5d9f9697U, 0xe523f1f2U,
    0x4d6b1905U, 0xf5d77e60U, 0xe762d18eU, 0x5fdeb6ebU, 0xc2098e52U,
    0x7ab5e937U, 0x680046d9U, 0xd0bc21bcU, 0x88df31eaU, 0x3063568fU,
    0x22d6f961U, 0x9a6a9e04U, 0x07bda6bdU, 0xbf01c1d8U, 0xadb46e36U,
    0x15080953U, 0x1d724e9aU, 0xa5ce29ffU, 0xb77b8611U, 0x0fc7e174U,
    0x9210d9cdU, 0x2aacbea8U, 0x38191146U, 0x80a57623U, 0xd8c66675U,
    0x607a0110U, 0x72cfaefeU, 0xca73c99bU, 0x57a4f122U, 0xef189647U,
    0xfdad39a9U, 0x45115eccU, 0x764dee06U, 0xcef18963U, 0xdc44268dU,
    0x64f841e8U, 0xf92f7951U, 0x41931e34U, 0x5326b1daU, 0xeb9ad6bfU,
    0xb3f9c6e9U, 0x0b45a18cU, 0x19f00e62U, 0xa14c6907U, 0x3c9b51beU,
    0x842736dbU, 0x96929935U, 0x2e2efe50U, 0x2654b999U, 0x9ee8defcU,
    0x8c5d7112U, 0x34e11677U, 0xa9362eceU, 0x118a49abU, 0x033fe645U,
    0xbb838120U, 0xe3e09176U, 0x5b5cf613U, 0x49e959fdU, 0xf1553e98U,
    0x6c820621U, 0xd43e6144U, 0xc68bceaaU, 0x7e37a9cfU, 0xd67f4138U,
    0x6ec3265dU, 0x7c7689b3U, 0xc4caeed6U, 0x591dd66fU, 0xe1a1b10aU,
    0xf3141ee4U, 0x4ba87981U, 0x13cb69d7U, 0xab770eb2U, 0xb9c2a15cU,
    0x017ec639U, 0x9ca9fe80U, 0x241599e5U, 0x36a0360bU, 0x8e1c516eU,
    0x866616a7U, 0x3eda71c2U, 0x2c6fde2cU, 0x94d3b949U, 0x090481f0U,
    0xb1b8e695U, 0xa30d497bU, 0x1bb12e1eU, 0x43d23e48U, 0xfb6e592dU,
    0xe9dbf6c3U, 0x516791a6U, 0xccb0a91fU, 0x740cce7aU, 0x66b96194U,
    0xde0506f1U
  },
  {
    0x00000000U, 0x96300777U, 0x2c610eeeU, 0xba510999U, 0x19c46d07U,
    0x8ff46a70U, 0x35a563e9U, 0xa395649eU, 0x3288db0eU, 0xa4b8dc79U,
    0x1ee9d5e0U, 0x88d9d297U, 0x2b4cb609U, 0xbd7cb17eU, 0x072db8e7U,
    0x911dbf90U, 0x6410b71dU, 0xf220b06aU, 0x4871b9f3U, 0xde41be84U,
    0x7dd4da1aU, 0xebe4dd6dU, 0x51b5d4f4U, 0xc785d383U, 0x56986c13U,
    0xc0a86b64U, 0x7af962fdU, 0xecc9658aU, 0x4f5c0114U, 0xd96c0663U,
    0x633d0ffaU, 0xf50d088dU, 0xc8206e3bU, 0x5e10694cU, 0xe44160d5U,
    0x727167a2U, 0xd1e4033cU, 0x47d4044bU, 0xfd850dd2U, 0x6bb50aa5U,
    0xfaa8b535U, 0x6c98b242U, 0xd6c9bbdbU, 0x40f9bcacU, 0xe36cd832U,
    0x755cdf45U, 0xcf0dd6dcU, 0x593dd1abU, 0xac30d926U, 0x3a00de51U,
    0x8051d7c8U, 0x1661d0bfU, 0xb5f4b421U, 0x23c4b356U, 0x9995bacfU,
    0x0fa5bdb8U, 0x9eb80228U, 0x0888055fU, 0xb2d90cc6U, 0x24e90bb1U,
    0x877c6f2fU, 0x114c6858U, 0xab1d61c1U, 0x3d2d66b6U, 0x9041dc76U,
    0x0671db01U, 0xbc20d298U, 0x2a10d5efU, 0x8985b171U, 0x1fb5b606U,
    0xa5e4bf9fU, 0x33d4b8e8U, 0xa2c90778U, 0x34f9000fU, 0x8ea80996U,
    0x18980ee1U, 0xbb0d6a7fU, 0x2d3d6d08U, 0x976c6491U, 0x015c63e6U,
    0xf4516b6bU, 0x62616c1cU, 0xd8306585U, 0x4e0062f2U, 0xed95066cU,
    0x7ba5011bU, 0xc1f40882U, 0x57c40ff5U, 0xc6d9b065U, 0x50e9b712U,
    0xeab8be8bU, 0x7c88b9fcU, 0xdf1ddd62U, 0x492dda15U, 0xf37cd38cU,
    0x654cd4fbU, 0x5861b24dU, 0xce51b53aU, 0x7400bca3U, 0xe230bbd4U,
    0x41a5df4aU, 0xd795d83dU, 0x6dc4d1a4U, 0xfbf4d6d3U, 0x6ae96943U,
    0xfcd96e34U, 0x468867adU, 0xd0b860daU, 0x732d0444U, 0xe51d0333U,
    0x5f4c0aaaU, 0xc97c0dddU, 0x3c710550U, 0xaa410227U, 0x10100bbeU,
    0x86200cc9U, 0x25b56857U, 0xb3856f20U, 0x09d466b9U, 0x9fe461ceU,
    0x0ef9de5eU, 0x98c9d929U, 0x2298d0b0U, 0xb4a8d7c7U, 0x173db359U,
    0x810db42eU, 0x3b5cbdb7U, 0xad6cbac0U, 0x2083b8edU, 0xb6b3bf9aU,
    0x0ce2b603U, 0x9ad2b174U, 0x3947d5eaU, 0xaf77d29dU, 0x1526db04U,
    0x8316dc73U, 0x120b63e3U, 0x843b6494U, 0x3e6a6d0dU, 0xa85a6a7aU,
    0x0bcf0ee4U, 0x9dff0993U, 0x27ae000aU, 0xb19e077dU, 0x44930ff0U,
    0xd2a30887U, 0x68f2011eU, 0xfec20669U, 0x5d5762f7U, 0xcb676580U,
    0x71366c19U, 0xe7066b6eU, 0x761bd4feU, 0xe02bd389U, 0x5a7ada10U,
    0xcc4add67U, 0x6fdfb9f9U, 0xf9efbe8eU, 0x43beb717U, 0xd58eb060U,
    0xe8a3d6d6U, 0x7e93d1a1U, 0xc4c2d838U, 0x52f2df4fU, 0xf167bbd1U,
    0x6757bca6U, 0xdd06b53fU, 0x4b36b248U, 0xda2b0dd8U, 0x4c1b0aafU,
    0xf64a0336U, 0x607a0441U, 0xc3ef60dfU, 0x55df67a8U, 0xef8e6e31U,
    0x79be6946U, 0x8cb361cbU, 0x1a8366bcU, 0xa0d26f25U, 0x36e26852U,
    0x95770cccU, 0x03470bbbU, 0xb9160222U, 0x2f260555U, 0xbe3bbac5U,
    0x280bbdb2U, 0x925ab42bU, 0x046ab35cU, 0xa7ffd7c2U, 0x31cfd0b5U,
    0x8b9ed92cU, 0x1daede5bU, 0xb0c2649bU, 0x26f263ecU, 0x9ca36a75U,
    0x0a936d02U, 0xa906099cU, 0x3f360eebU, 0x85670772U, 0x13570005U,
    0x824abf95U, 0x147ab8e2U, 0xae2bb17bU, 0x381bb60cU, 0x9b8ed292U,
    0x0dbed5e5U, 0xb7efdc7cU, 0x21dfdb0bU, 0xd4d2d386U, 0x42e2d4f1U,
    0xf8b3dd68U, 0x6e83da1fU, 0xcd16be81U, 0x5b26b9f6U, 0xe177b06fU,
    0x7747b718U, 0xe65a0888U, 0x706a0fffU, 0xca3b0666U, 0x5c0b0111U,
    0xff9e658fU, 0x69ae62f8U, 0xd3ff6b61U, 0x45cf6c16U, 0x78e20aa0U,
    0xeed20dd7U, 0x5483044eU, 0xc2b30339U, 0x612667a7U, 0xf71660d0U,
    0x4d476949U, 0xdb776e3eU, 0x4a6ad1aeU, 0xdc5ad6d9U, 0x660bdf40U,
    0xf03bd837U, 0x53aebca9U, 0xc59ebbdeU, 0x7fcfb247U, 0xe9ffb530U,
    0x1cf2bdbdU, 0x8ac2bacaU, 0x3093b353U, 0xa6a3b424U, 0x0536d0baU,
    0x9306d7cdU, 0x2957de54U, 0xbf67d923U, 0x2e7a66b3U, 0xb84a61c4U,
    0x021b685dU, 0x942b6f2aU, 0x37be0bb4U, 0xa18e0cc3U, 0x1bdf055aU,
    0x8def022dU
  },
  {
    0x00000000U, 0x41311b19U, 0x82623632U, 0xc3532d2bU, 0x04c56c64U,
    0x45f4777dU, 0x86a75a56U, 0xc796414fU, 0x088ad9c8U, 0x49bbc2d1U,
    0x8ae8effaU, 0xcbd9f4e3U, 0x0c4fb5acU, 0x4d7eaeb5U, 0x8e2d839eU,
    0xcf1c9887U, 0x5112c24aU, 0x1023d953U, 0xd370f478U, 0x9241ef61U,
    0x55d7ae2eU, 0x14e6b537U, 0xd7b5981cU, 0x96848305U, 0x59981b82U,
    0x18a9009bU, 0xdbfa2db0U, 0x9acb36a9U, 0x5d5d77e6U, 0x1c6c6cffU,
    0xdf3f41d4U, 0x9e0e5acdU, 0xa2248495U, 0xe3159f8cU, 0x2046b2a7U,
    0x6177a9beU, 0xa6e1e8f1U, 0xe7d0f3e8U, 0x2483dec3U, 0x65b2c5daU,
    0xaaae5d5dU, 0xeb9f4644U, 0x28cc6b6fU, 0x69fd7076U, 0xae6b3139U,
    0xef5a2a20U, 0x2c09070bU, 0x6d381c12U, 0xf33646dfU, 0xb2075dc6U,
    0x715470edU, 0x30656bf4U, 0xf7f32abbU, 0xb6c231a2U, 0x75911c89U,
    0x34a00790U, 0xfbbc9f17U, 0xba8d840eU, 0x79dea925U, 0x38efb23cU,
    0xff79f373U, 0xbe48e86aU, 0x7d1bc541U, 0x3c2ade58U, 0x054f79f0U,
    0x447e62e9U, 0x872d4fc2U, 0xc61c54dbU, 0x018a1594U, 0x40bb0e8dU,
    0x83e823a6U, 0xc2d938bfU, 0x0dc5a038U, 0x4cf4bb21U, 0x8fa7960aU,
    0xce968d13U, 0x0900cc5cU, 0x4831d745U, 0x8b62fa6eU, 0xca53e177U,
    0x545dbbbaU, 0x156ca0a3U, 0xd63f8d88U, 0x970e9691U, 0x5098d7deU,
    0x11a9ccc7U, 0xd2fae1ecU, 0x93cbfaf5U, 0x5cd76272U, 0x1de6796bU,
    0xdeb55440U, 0x9f844f59U, 0x58120e16U, 0x1923150fU, 0xda703824U,
    0x9b41233dU, 0xa76bfd65U, 0xe65ae67cU, 0x2509cb57U, 0x6438d04eU,
    0xa3ae9101U, 0xe29f8a18U, 0x21cca733U, 0x60fdbc2aU, 0xafe124adU,
    0xeed03fb4U, 0x2d83129fU, 0x6cb20986U, 0xab2448c9U, 0xea1553d0U,
    0x29467efbU, 0x687765e2U, 0xf6793f2fU, 0xb7482436U, 0x741b091dU,
    0x352a1204U, 0xf2bc534bU, 0xb38d4852U, 0x70de6579U, 0x31ef7e60U,
    0xfef3e6e7U, 0xbfc2fdfeU, 0x7c91d0d5U, 0x3da0cbccU, 0xfa368a83U,
    0xbb07919aU, 0x7854bcb1U, 0x3965a7a8U, 0x4b98833bU, 0x0aa99822U,
    0xc9fab509U, 0x88cbae10U, 0x4f5def5fU, 0x0e6cf446U, 0xcd3fd96dU,
    0x8c0ec274U, 0x43125af3U, 0x022341eaU, 0xc1706cc1U, 0x804177d8U,
    0x47d73697U, 0x06e62d8eU, 0xc5b500a5U, 0x84841bbcU, 0x1a8a4171U,
    0x5bbb5a68U, 0x98e87743U, 0xd9d96c5aU, 0x1e4f2d15U, 0x5f7e360cU,
    0x9c2d1b27U, 0xdd1c003eU, 0x120098b9U, 0x533183a0U, 0x9062ae8bU,
    0xd153b592U, 0x16c5f4ddU, 0x57f4efc4U, 0x94a7c2efU, 0xd596d9f6U,
    0xe9bc07aeU, 0xa88d1cb7U, 0x6bde319cU, 0x2aef2a85U, 0xed796bcaU,
    0xac4870d3U, 0x6f1b5df8U, 0x2e2a46e1U, 0xe136de66U, 0xa007c57fU,
    0x6354e854U, 0x2265f34dU, 0xe5f3b202U, 0xa4c2a91bU, 0x67918430U,
    0x26a09f29U, 0xb8aec5e4U, 0xf99fdefdU, 0x3accf3d6U, 0x7bfde8cfU,
    0xbc6ba980U, 0xfd5ab299U, 0x3e099fb2U, 0x7f3884abU, 0xb0241c2cU,
    0xf1150735U, 0x32462a1eU, 0x73773107U, 0xb4e17048U, 0xf5d06b51U,
    0x3683467aU, 0x77b25d63U, 0x4ed7facbU, 0x0fe6e1d2U, 0xccb5ccf9U,
    0x8d84d7e0U, 0x4a1296afU, 0x0b238db6U, 0xc870a09dU, 0x8941bb84U,
    0x465d2303U, 0x076c381aU, 0xc43f1531U, 0x850e0e28U, 0x42984f67U,
    0x03a9547eU, 0xc0fa7955U, 0x81cb624cU, 0x1fc53881U, 0x5ef42398U,
    0x9da70eb3U, 0xdc9615aaU, 0x1b0054e5U, 0x5a314ffcU, 0x996262d7U,
    0xd85379ceU, 0x174fe149U, 0x567efa50U, 0x952dd77bU, 0xd41ccc62U,
    0x138a8d2dU, 0x52bb9634U, 0x91e8bb1fU, 0xd0d9a006U, 0xecf37e5eU,
    0xadc26547U, 0x6e91486cU, 0x2fa05375U, 0xe836123aU, 0xa9070923U,
    0x6a542408U, 0x2b653f11U, 0xe479a796U, 0xa548bc8fU, 0x661b91a4U,
    0x272a8abdU, 0xe0bccbf2U, 0xa18dd0ebU, 0x62defdc0U, 0x23efe6d9U,
    0xbde1bc14U, 0xfcd0a70dU, 0x3f838a26U, 0x7eb2913fU, 0xb924d070U,
    0xf815cb69U, 0x3b46e642U, 0x7a77fd5bU, 0xb56b65dcU, 0xf45a7ec5U,
    0x370953eeU, 0x763848f7U, 0xb1ae09b8U, 0xf09f12a1U, 0x33cc3f8aU,
    0x72fd2493U
  },
  {
    0x00000000U, 0x376ac201U, 0x6ed48403U, 0x59be4602U, 0xdca80907U,
    0xebc2cb06U, 0xb27c8d04U, 0x85164f05U, 0xb851130eU, 0x8f3bd10fU,
    0xd685970dU, 0xe1ef550cU, 0x64f91a09U, 0x5393d808U, 0x0a2d9e0aU,
    0x3d475c0bU, 0x70a3261cU, 0x47c9e41dU, 0x1e77a21fU, 0x291d601eU,
    0xac0b2f1bU, 0x9b61ed1aU, 0xc2dfab18U, 0xf5b56919U, 0xc8f23512U,
    0xff98f713U, 0xa626b111U, 0x914c7310U, 0x145a3c15U, 0x2330fe14U,
    0x7a8eb816U, 0x4de47a17U, 0xe0464d38U, 0xd72c8f39U, 0x8e92c93bU,
    0xb9f80b3aU, 0x3cee443fU, 0x0b84863eU, 0x523ac03cU, 0x6550023dU,
    0x58175e36U, 0x6f7d9c37U, 0x36c3da35U, 0x01a91834U, 0x84bf5731U,
    0xb3d59530U, 0xea6bd332U, 0xdd011133U, 0x90e56b24U, 0xa78fa925U,
    0xfe31ef27U, 0xc95b2d26U, 0x4c4d6223U, 0x7b27a022U, 0x2299e620U,
    0x15f32421U, 0x28b4782aU, 0x1fdeba2bU, 0x4660fc29U, 0x710a3e28U,
    0xf41c712dU, 0xc376b32cU, 0x9ac8f52eU, 0xada2372fU, 0xc08d9a70U,
    0xf7e75871U, 0xae591e73U, 0x9933dc72U, 0x1c259377U, 0x2b4f5176U,
    0x72f11774U, 0x459bd575U, 0x78dc897eU, 0x4fb64b7fU, 0x16080d7dU,
    0x2162cf7cU, 0xa4748079U, 0x931e4278U, 0xcaa0047aU, 0xfdcac67bU,
    0xb02ebc6cU, 0x87447e6dU, 0xdefa386fU, 0xe990fa6eU, 0x6c86b56bU,
    0x5bec776aU, 0x02523168U, 0x3538f369U, 0x087faf62U, 0x3f156d63U,
    0x66ab2b61U, 0x51c1e960U, 0xd4d7a665U, 0xe3bd6464U, 0xba032266U,
    0x8d69e067U, 0x20cbd748U, 0x17a11549U, 0x4e1f534bU, 0x7975914aU,
    0xfc63de4fU, 0xcb091c4eU, 0x92b75a4cU, 0xa5dd984dU, 0x989ac446U,
    0xaff00647U, 0xf64e4045U, 0xc1248244U, 0x4432cd41U, 0x73580f40U,
    0x2ae64942U, 0x1d8c8b43U, 0x5068f154U, 0x67023355U, 0x3ebc7557U,
    0x09d6b756U, 0x8cc0f853U, 0xbbaa3a52U, 0xe2147c50U, 0xd57ebe51U,
    0xe839e25aU, 0xdf53205bU, 0x86ed6659U, 0xb187a458U, 0x3491eb5dU,
    0x03fb295cU, 0x5a456f5eU, 0x6d2fad5fU, 0x801b35e1U, 0xb771f7e0U,
    0xeecfb1e2U, 0xd9a573e3U, 0x5cb33ce6U, 0x6bd9fee7U, 0x3267b8e5U,
    0x050d7ae4U, 0x384a26efU, 0x0f20e4eeU, 0x569ea2ecU, 0x61f460edU,
    0xe4e22fe8U, 0xd388ede9U, 0x8a36abebU, 0xbd5c69eaU, 0xf0b813fdU,
    0xc7d2d1fcU, 0x9e6c97feU, 0xa90655ffU, 0x2c101afaU, 0x1b7ad8fbU,
    0x42c49ef9U, 0x75ae5cf8U, 0x48e900f3U, 0x7f83c2f2U, 0x263d84f0U,
    0x115746f1U, 0x944109f4U, 0xa32bcbf5U, 0xfa958df7U, 0xcdff4ff6U,
    0x605d78d9U, 0x5737bad8U, 0x0e89fcdaU, 0x39e33edbU, 0xbcf571deU,
    0x8b9fb3dfU, 0xd221f5ddU, 0xe54b37dcU, 0xd80c6bd7U, 0xef66a9d6U,
    0xb6d8efd4U, 0x81b22dd5U, 0x04a462d0U, 0x33cea0d1U, 0x6a70e6d3U,
    0x5d1a24d2U, 0x10fe5ec5U, 0x27949cc4U, 0x7e2adac6U, 0x494018c7U,
    0xcc5657c2U, 0xfb3c95c3U, 0xa282d3c1U, 0x95e811c0U, 0xa8af4dcbU,
    0x9fc58fcaU, 0xc67bc9c8U, 0xf1110bc9U, 0x740744ccU, 0x436d86cdU,
    0x1ad3c0cfU, 0x2db902ceU, 0x4096af91U, 0x77fc6d90U, 0x2e422b92U,
    0x1928e993U, 0x9c3ea696U, 0xab546497U, 0xf2ea2295U, 0xc580e094U,
    0xf8c7bc9fU, 0xcfad7e9eU, 0x9613389cU, 0xa179fa9dU, 0x246fb598U,
    0x13057799U, 0x4abb319bU, 0x7dd1f39aU, 0x3035898dU, 0x075f4b8cU,
    0x5ee10d8eU, 0x698bcf8fU, 0xec9d808aU, 0xdbf7428bU, 0x82490489U,
    0xb523c688U, 0x88649a83U, 0xbf0e5882U, 0xe6b01e80U, 0xd1dadc81U,
    0x54cc9384U, 0x63a65185U, 0x3a181787U, 0x0d72d586U, 0xa0d0e2a9U,
    0x97ba20a8U, 0xce0466aaU, 0xf96ea4abU, 0x7c78ebaeU, 0x4b1229afU,
    0x12ac6fadU, 0x25c6adacU, 0x1881f1a7U, 0x2feb33a6U, 0x765575a4U,
    0x413fb7a5U, 0xc429f8a0U, 0xf3433aa1U, 0xaafd7ca3U, 0x9d97bea2U,
    0xd073c4b5U, 0xe71906b4U, 0xbea740b6U, 0x89cd82b7U, 0x0cdbcdb2U,
    0x3bb10fb3U, 0x620f49b1U, 0x55658bb0U, 0x6822d7bbU, 0x5f4815baU,
    0x06f653b8U, 0x319c91b9U, 0xb48adebcU, 0x83e01cbdU, 0xda5e5abfU,
    0xed3498beU
  },
  {
    0x00000000U, 0x6567bcb8U, 0x8bc809aaU, 0xeeafb512U, 0x5797628fU,
    0x32f0de37U, 0xdc5f6b25U, 0xb938d79dU, 0xef28b4c5U, 0x8a4f087dU,
    0x64e0bd6fU, 0x018701d7U, 0xb8bfd64aU, 0xddd86af2U, 0x3377dfe0U,
    0x56106358U, 0x9f571950U, 0xfa30a5e8U, 0x149f10faU, 0x71f8ac42U,
    0xc8c07bdfU, 0xada7c767U, 0x43087275U, 0x266fcecdU, 0x707fad95U,
    0x1518112dU, 0xfbb7a43fU, 0x9ed01887U, 0x27e8cf1aU, 0x428f73a2U,
    0xac20c6b0U, 0xc9477a08U, 0x3eaf32a0U, 0x5bc88e18U, 0xb5673b0aU,
    0xd00087b2U, 0x6938502fU, 0x0c5fec97U, 0xe2f05985U, 0x8797e53dU,
    0xd1878665U, 0xb4e03addU, 0x5a4f8fcfU, 0x3f283377U, 0x8610e4eaU,
    0xe3775852U, 0x0dd8ed40U, 0x68bf51f8U, 0xa1f82bf0U, 0xc49f9748U,
    0x2a30225aU, 0x4f579ee2U, 0xf66f497fU, 0x9308f5c7U, 0x7da740d5U,
    0x18c0fc6dU, 0x4ed09f35U, 0x2bb7238dU, 0xc518969fU, 0xa07f2a27U,
    0x1947fdbaU, 0x7c204102U, 0x928ff410U, 0xf7e848a8U, 0x3d58149bU,
    0x583fa823U, 0xb6901d31U, 0xd3f7a189U, 0x6acf7614U, 0x0fa8caacU,
    0xe1077fbeU, 0x8460c306U, 0xd270a05eU, 0xb7171ce6U, 0x59b8a9f4U,
    0x3cdf154cU, 0x85e7c2d1U, 0xe0807e69U, 0x0e2fcb7bU, 0x6b4877c3U,
    0xa20f0dcbU, 0xc768b173U, 0x29c70461U, 0x4ca0b8d9U, 0xf5986f44U,
    0x90ffd3fcU, 0x7e5066eeU, 0x1b37da56U, 0x4d27b90eU, 0x284005b6U,
    0xc6efb0a4U, 0xa3880c1cU, 0x1ab0db81U, 0x7fd76739U, 0x9178d22bU,
    0xf41f6e93U, 0x03f7263bU, 0x66909a83U, 0x883f2f91U, 0xed589329U,
    0x546044b4U, 0x3107f80cU, 0xdfa84d1eU, 0xbacff1a6U, 0xecdf92feU,
    0x89b82e46U, 0x67179b54U, 0x027027ecU, 0xbb48f071U, 0xde2f4cc9U,
    0x3080f9dbU, 0x55e74563U, 0x9ca03f6bU, 0xf9c783d3U, 0x176836c1U,
    0x720f8a79U, 0xcb375de4U, 0xae50e15cU, 0x40ff544eU, 0x2598e8f6U,
    0x73888baeU, 0x16ef3716U, 0xf8408204U, 0x9d273ebcU, 0x241fe921U,
    0x41785599U, 0xafd7e08bU, 0xcab05c33U, 0x3bb659edU, 0x5ed1e555U,
    0xb07e5047U, 0xd519ecffU, 0x6c213b62U, 0x094687daU, 0xe7e932c8U,
    0x828e8e70U, 0xd49eed28U, 0xb1f95190U, 0x5f56e482U, 0x3a31583aU,
    0x83098fa7U, 0xe66e331fU, 0x08c1860dU, 0x6da63ab5U, 0xa4e140bdU,
    0xc186fc05U, 0x2f294917U, 0x4a4ef5afU, 0xf3762232U, 0x96119e8aU,
    0x78be2b98U, 0x1dd99720U, 0x4bc9f478U, 0x2eae48c0U, 0xc001fdd2U,
    0xa566416aU, 0x1c5e96f7U, 0x79392a4fU, 0x97969f5dU, 0xf2f123e5U,
    0x05196b4dU, 0x607ed7f5U, 0x8ed162e7U, 0xebb6de5fU, 0x528e09c2U,
    0x37e9b57aU, 0xd9460068U, 0xbc21bcd0U, 0xea31df88U, 0x8f566330U,
    0x61f9d622U, 0x049e6a9aU, 0xbda6bd07U, 0xd8c101bfU, 0x366eb4adU,
    0x53090815U, 0x9a4e721dU, 0xff29cea5U, 0x11867bb7U, 0x74e1c70fU,
    0xcdd91092U, 0xa8beac2aU, 0x46111938U, 0x2376a580U, 0x7566c6d8U,
    0x10017a60U, 0xfeaecf72U, 0x9bc973caU, 0x22f1a457U, 0x479618efU,
    0xa939adfdU, 0xcc5e1145U, 0x06ee4d76U, 0x6389f1ceU, 0x8d2644dcU,
    0xe841f864U, 0x51792ff9U, 0x341e9341U, 0xdab12653U, 0xbfd69aebU,
    0xe9c6f9b3U, 0x8ca1450bU, 0x620ef019U, 0x07694ca1U, 0xbe519b3cU,
    0xdb362784U, 0x35999296U, 0x50fe2e2eU, 0x99b95426U, 0xfcdee89eU,
    0x12715d8cU, 0x7716e134U, 0xce2e36a9U, 0xab498a11U, 0x45e63f03U,
    0x208183bbU, 0x7691e0e3U, 0x13f65c5bU, 0xfd59e949U, 0x983e55f1U,
    0x2106826cU, 0x44613ed4U, 0xaace8bc6U, 0xcfa9377eU, 0x38417fd6U,
    0x5d26c36eU, 0xb389767cU, 0xd6eecac4U, 0x6fd61d59U, 0x0ab1a1e1U,
    0xe41e14f3U, 0x8179a84bU, 0xd769cb13U, 0xb20e77abU, 0x5ca1c2b9U,
    0x39c67e01U, 0x80fea99cU, 0xe5991524U, 0x0b36a036U, 0x6e511c8eU,
    0xa7166686U, 0xc271da3eU, 0x2cde6f2cU, 0x49b9d394U, 0xf0810409U,
    0x95e6b8b1U, 0x7b490da3U, 0x1e2eb11bU, 0x483ed243U, 0x2d596efbU,
    0xc3f6dbe9U, 0xa6916751U, 0x1fa9b0ccU, 0x7ace0c74U, 0x9461b966U,
    0xf10605deU
  }
};

#define PCLMUL_MIN_LEN 64
#define PCLMUL_ALIGN 16
#define PCLMUL_ALIGN_MASK 15

#if defined(__x86_64__) || defined(__amd64__)
/* crc32_simd.c
 *
 * Copyright 2017 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 *
 * crc32_sse42_simd_(): compute the crc32 of the buffer, where the buffer
 * length must be at least 64, and a multiple of 16. Based on:
 *
 * "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *  V. Gopal, E. Ozturk, et al., 2009, http://intel.ly/2ySEwL0
 */
static uint32_t
__attribute__((target("pclmul,sse4.1")))
crc32pclmul(uint32_t crc, const uint8_t *buf, size_t len)
{
    /*
     * Definitions of the bit-reflected domain constants k1,k2,k3, etc and
     * the CRC32+Barrett polynomials given at the end of the paper.
     */
    static const uint64_t zalign(16) k1k2[] = { 0x0154442bd4, 0x01c6e41596 };
    static const uint64_t zalign(16) k3k4[] = { 0x01751997d0, 0x00ccaa009e };
    static const uint64_t zalign(16) k5k0[] = { 0x0163cd6124, 0x0000000000 };
    static const uint64_t zalign(16) poly[] = { 0x01db710641, 0x01f7011641 };

    __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;

    /*
     * There's at least one block of 64.
     */
    x1 = _mm_loadu_si128((__m128i *)(buf + 0x00));
    x2 = _mm_loadu_si128((__m128i *)(buf + 0x10));
    x3 = _mm_loadu_si128((__m128i *)(buf + 0x20));
    x4 = _mm_loadu_si128((__m128i *)(buf + 0x30));

    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128(crc));

    x0 = _mm_load_si128((__m128i *)k1k2);

    buf += 64;
    len -= 64;

    /*
     * Parallel fold blocks of 64, if any.
     */
    while (len >= 64)
    {
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        x8 = _mm_clmulepi64_si128(x4, x0, 0x00);

        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
        x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
        x4 = _mm_clmulepi64_si128(x4, x0, 0x11);

        y5 = _mm_loadu_si128((__m128i *)(buf + 0x00));
        y6 = _mm_loadu_si128((__m128i *)(buf + 0x10));
        y7 = _mm_loadu_si128((__m128i *)(buf + 0x20));
        y8 = _mm_loadu_si128((__m128i *)(buf + 0x30));

        x1 = _mm_xor_si128(x1, x5);
        x2 = _mm_xor_si128(x2, x6);
        x3 = _mm_xor_si128(x3, x7);
        x4 = _mm_xor_si128(x4, x8);

        x1 = _mm_xor_si128(x1, y5);
        x2 = _mm_xor_si128(x2, y6);
        x3 = _mm_xor_si128(x3, y7);
        x4 = _mm_xor_si128(x4, y8);

        buf += 64;
        len -= 64;
    }

    /*
     * Fold into 128-bits.
     */
    x0 = _mm_load_si128((__m128i *)k3k4);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x2);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x3);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x4);
    x1 = _mm_xor_si128(x1, x5);

    /*
     * Single fold blocks of 16, if any.
     */
    while (len >= 16)
    {
        x2 = _mm_loadu_si128((__m128i *)buf);

        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x1 = _mm_xor_si128(x1, x2);
        x1 = _mm_xor_si128(x1, x5);

        buf += 16;
        len -= 16;
    }

    /*
     * Fold 128-bits to 64-bits.
     */
    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_srli_si128(x1, 8);
    x1 = _mm_xor_si128(x1, x2);

    x0 = _mm_loadl_epi64((__m128i*)k5k0);

    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_and_si128(x1, x3);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    /*
     * Barret reduce to 32-bits.
     */
    x0 = _mm_load_si128((__m128i*)poly);

    x2 = _mm_and_si128(x1, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x10);
    x2 = _mm_and_si128(x2, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    /*
     * Return the crc32.
     */
    return _mm_extract_epi32(x1, 1);
}
#endif

/*
   This BYFOUR code accesses the passed unsigned char * buffer with a 32-bit
   integer pointer type. This violates the strict aliasing rule, where a
   compiler can assume, for optimization purposes, that two pointers to
   fundamentally different types won't ever point to the same memory. This can
   manifest as a problem only if one of the pointers is written to. This code
   only reads from those pointers. So long as this code remains isolated in
   this compilation unit, there won't be a problem. For this reason, this code
   should not be copied and pasted into a compilation unit in which other code
   writes to the buffer that is passed to these routines.
 */

#ifdef DNBD3_LITTLE_ENDIAN
/* ========================================================================= */
#define DOLIT4 c ^= *buf4++; \
        c = crc_table[3][c & 0xff] ^ crc_table[2][(c >> 8) & 0xff] ^ \
            crc_table[1][(c >> 16) & 0xff] ^ crc_table[0][c >> 24]
#define DOLIT32 DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4

/* ========================================================================= */
uint32_t crc32(crc, buf, len)
    uint32_t crc;
    const uint8_t *buf;
    size_t len;
{
    if (buf == NULL) return 0;
    uint32_t c;

    c = ~crc;
    while (len && ((uintptr_t)buf & PCLMUL_ALIGN_MASK)) {
        c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
        len--;
    }
#if defined(__x86_64__) || defined(__amd64__)
    static  atomic_int pclmul = -1;
    if (pclmul == -1) {
        pclmul = __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.1");
    }
    if (pclmul && len >= PCLMUL_MIN_LEN) {
        c = crc32pclmul(c, buf, len & ~PCLMUL_ALIGN_MASK);
        buf += len & ~PCLMUL_ALIGN_MASK;
        len &= PCLMUL_ALIGN_MASK;
    } else
#endif
    do {
        const uint32_t *buf4 = (const uint32_t *)(const void *)buf;
        while (len >= 32) {
            DOLIT32;
            len -= 32;
        }
        while (len >= 4) {
            DOLIT4;
            len -= 4;
        }
        buf = (const uint8_t *)buf4;
    } while (0);

    if (len) do {
        c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
    } while (--len);
    c = ~c;
    return c;
}
#endif

#ifdef DNBD3_BIG_ENDIAN
/* ========================================================================= */
#define DOBIG4 c ^= *buf4++; \
        c = crc_table[4][c & 0xff] ^ crc_table[5][(c >> 8) & 0xff] ^ \
            crc_table[6][(c >> 16) & 0xff] ^ crc_table[7][c >> 24]
#define DOBIG32 DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4

/* ========================================================================= */
uint32_t crc32(crc, buf, len)
    uint32_t crc;
    const uint8_t *buf;
    size_t len;
{
    if (buf == NULL) return 0;
    register uint32_t c;
    register const uint32_t *buf4;

    c = ~net_order_32(crc);
    while (len && ((uintptr_t)buf & 3)) {
        c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
        len--;
    }

    buf4 = (const uint32_t *)(const void *)buf;
    while (len >= 32) {
        DOBIG32;
        len -= 32;
    }
    while (len >= 4) {
        DOBIG4;
        len -= 4;
    }
    buf = (const uint8_t *)buf4;

    if (len) do {
        c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
    } while (--len);
    c = ~c;
    return net_order_32(c);
}
#endif

