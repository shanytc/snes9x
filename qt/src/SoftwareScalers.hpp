#pragma once

void S9xForceHires(void *buffer, int pitch, int &width, int &height);
void S9xMergeHires(void *buffer, int pitch, int &width, int &height);
/* In-place horizontal blend of a 512-wide frame that keeps its width: each
 * pixel becomes the average of itself and its left neighbour (win32's
 * RenderMergeHires). */
void S9xBlendHires(void *buffer, int pitch, int width, int height);
