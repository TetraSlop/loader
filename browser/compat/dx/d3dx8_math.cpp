// D3DX uses row vectors: a point transforms as v * M, so M1 * M2 applies M1 first.
#include <d3dx8.h>

D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* out, const D3DXMATRIX* m1, const D3DXMATRIX* m2)
{
    D3DXMATRIX product;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            product.m[row][column] = m1->m[row][0] * m2->m[0][column] + m1->m[row][1] * m2->m[1][column] + m1->m[row][2] * m2->m[2][column] + m1->m[row][3] * m2->m[3][column];
        }
    }

    *out = product;
    return out;
}
