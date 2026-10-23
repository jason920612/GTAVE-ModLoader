#include "GTAVDds.h"

namespace
{
	// RGB565 -> 8-bit channels.
	void Unpack565(uint16 C, int32 Out[3])
	{
		Out[0] = ((C >> 11) & 31) * 255 / 31;
		Out[1] = ((C >> 5) & 63) * 255 / 63;
		Out[2] = (C & 31) * 255 / 31;
	}

	// The colour part of BC1/2/3: 16 pixels of RGBA (alpha 255, or 0 for BC1's transparent index).
	void DecodeColor(const uint8* B, bool bFourColors, uint8 Out[16][4])
	{
		const uint16 C0 = B[0] | B[1] << 8, C1 = B[2] | B[3] << 8;
		int32 P[4][4];
		Unpack565(C0, P[0]);
		Unpack565(C1, P[1]);
		P[0][3] = P[1][3] = 255;
		if (bFourColors || C0 > C1)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				P[2][K] = (2 * P[0][K] + P[1][K]) / 3;
				P[3][K] = (P[0][K] + 2 * P[1][K]) / 3;
			}
			P[2][3] = P[3][3] = 255;
		}
		else
		{
			for (int32 K = 0; K < 3; ++K)
			{
				P[2][K] = (P[0][K] + P[1][K]) / 2;
				P[3][K] = 0;
			}
			P[2][3] = 255;
			P[3][3] = 0;
		}
		const uint32 Indices = B[4] | B[5] << 8 | B[6] << 16 | uint32(B[7]) << 24;
		for (int32 I = 0; I < 16; ++I)
		{
			const int32 X = (Indices >> (2 * I)) & 3;
			for (int32 K = 0; K < 4; ++K)
			{
				Out[I][K] = uint8(P[X][K]);
			}
		}
	}

	// A BC4 block (also BC3's alpha, BC5's channels): 16 values.
	void DecodeChannel(const uint8* B, uint8 Out[16])
	{
		int32 V[8];
		V[0] = B[0];
		V[1] = B[1];
		if (V[0] > V[1])
		{
			for (int32 K = 1; K < 7; ++K)
			{
				V[K + 1] = ((7 - K) * V[0] + K * V[1]) / 7;
			}
		}
		else
		{
			for (int32 K = 1; K < 5; ++K)
			{
				V[K + 1] = ((5 - K) * V[0] + K * V[1]) / 5;
			}
			V[6] = 0;
			V[7] = 255;
		}
		uint64 Bits = 0;
		for (int32 K = 0; K < 6; ++K)
		{
			Bits |= uint64(B[2 + K]) << (8 * K);
		}
		for (int32 I = 0; I < 16; ++I)
		{
			Out[I] = uint8(V[(Bits >> (3 * I)) & 7]);
		}
	}
}

bool GTAVDds::Decode(const TArray<uint8>& File, bool bNormalMap, int32& OutWidth, int32& OutHeight, TArray<FColor>& OutPixels, FString& OutError)
{
	if (File.Num() < 148 || FMemory::Memcmp(File.GetData(), "DDS ", 4) != 0)
	{
		OutError = TEXT("not a DDS");
		return false;
	}
	const auto U32 = [&](int32 At) { return uint32(File[At]) | uint32(File[At + 1]) << 8 | uint32(File[At + 2]) << 16 | uint32(File[At + 3]) << 24; };
	const int32 H = U32(12), W = U32(16);
	const uint32 Format = U32(84) == 0x30315844 /* DX10 */ ? U32(128) : 0;
	const uint8* Data = File.GetData() + 148;
	const int64 Size = File.Num() - 148;
	OutWidth = W;
	OutHeight = H;
	OutPixels.SetNumZeroed(W * H);
	const auto Put = [&](int32 X, int32 Y, uint8 R, uint8 G, uint8 B, uint8 A) {
		if (X < W && Y < H)
		{
			OutPixels[Y * W + X] = FColor(R, G, B, A);
		}
	};
	if (Format == 65) // A8
	{
		if (Size < int64(W) * H)
		{
			OutError = TEXT("cut short");
			return false;
		}
		for (int32 I = 0; I < W * H; ++I)
		{
			OutPixels[I] = FColor(255, 255, 255, Data[I]);
		}
		return true;
	}
	if (Format == 87 || Format == 28) // B8G8R8A8 / R8G8B8A8
	{
		if (Size < int64(W) * H * 4)
		{
			OutError = TEXT("cut short");
			return false;
		}
		for (int32 I = 0; I < W * H; ++I)
		{
			const uint8* P = Data + 4 * I;
			OutPixels[I] = Format == 87 ? FColor(P[2], P[1], P[0], P[3]) : FColor(P[0], P[1], P[2], P[3]);
		}
		return true;
	}
	int32 BlockBytes;
	switch (Format)
	{
	case 71: // BC1
	case 80: // BC4
		BlockBytes = 8;
		break;
	case 74: // BC2
	case 77: // BC3
	case 83: // BC5
		BlockBytes = 16;
		break;
	default:
		OutError = FString::Printf(TEXT("DXGI format %u not decoded yet"), Format);
		return false;
	}
	const int32 BW = FMath::Max(1, (W + 3) / 4), BH = FMath::Max(1, (H + 3) / 4);
	if (Size < int64(BW) * BH * BlockBytes)
	{
		OutError = TEXT("cut short");
		return false;
	}
	for (int32 By = 0; By < BH; ++By)
	{
		for (int32 Bx = 0; Bx < BW; ++Bx)
		{
			const uint8* B = Data + (int64(By) * BW + Bx) * BlockBytes;
			uint8 Px[16][4];
			uint8 Ch[16], Ch2[16];
			switch (Format)
			{
			case 71:
				DecodeColor(B, false, Px);
				break;
			case 74:
				DecodeColor(B + 8, true, Px);
				for (int32 I = 0; I < 16; ++I)
				{
					Px[I][3] = uint8(((B[I / 2] >> (4 * (I & 1))) & 15) * 17);
				}
				break;
			case 77:
				DecodeColor(B + 8, true, Px);
				DecodeChannel(B, Ch);
				for (int32 I = 0; I < 16; ++I)
				{
					Px[I][3] = Ch[I];
				}
				break;
			case 80:
				DecodeChannel(B, Ch);
				for (int32 I = 0; I < 16; ++I)
				{
					Px[I][0] = Px[I][1] = Px[I][2] = Ch[I];
					Px[I][3] = 255;
				}
				break;
			case 83:
				DecodeChannel(B, Ch);
				DecodeChannel(B + 8, Ch2);
				for (int32 I = 0; I < 16; ++I)
				{
					Px[I][0] = Ch[I];
					Px[I][1] = Ch2[I];
					Px[I][2] = 255;
					if (bNormalMap)
					{
						const float Nx = Ch[I] / 127.5f - 1.0f, Ny = Ch2[I] / 127.5f - 1.0f;
						Px[I][2] = uint8(FMath::Clamp((FMath::Sqrt(FMath::Max(0.0f, 1.0f - Nx * Nx - Ny * Ny)) + 1.0f) * 127.5f, 0.0f, 255.0f));
					}
					Px[I][3] = 255;
				}
				break;
			}
			for (int32 I = 0; I < 16; ++I)
			{
				Put(Bx * 4 + (I & 3), By * 4 + (I >> 2), Px[I][0], Px[I][1], Px[I][2], Px[I][3]);
			}
		}
	}
	return true;
}
