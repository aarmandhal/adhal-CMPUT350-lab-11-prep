#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include <algorithm>  // std::max, std::clamp
#include <cassert>
#include <cstdint>  // SIZE_MAX
#include <iostream>
#include <vector>

#include "vector_util.h"
#include <SFML/Graphics.hpp>

using uint = unsigned int;

// RectPacker maintains two offsets into an overall bin size:
//  - A line y-offset, representing the current horizontal line on which to place new rectangles
//      horizontally along (the line corresponding to the rectangle tops)
//  - An x-offset into the line, representing the x-coordinate to align the next rectangle left-side
// Furthermore it maintains the maximum texture height encountered along the current line.
// The RectPacker places rectangles left to right along this line until it cannot fit the next
//  rectangle. Once this happens, the line is shifted down to meet the bottom of the tallest texture
//  placed on the line, and the x-offset is reset. This continues until the next rectangle
//  packed cannot be fit on the line within the bin space.
class RectPacker {
public:
    // Store the bin size and start the first shelf at the origin with zero height so far.
    RectPacker(const sf::Vector2f& size)
        : mBinSize(size), mShelfY(0.f), mCursorX(0.f), mShelfMaxHeight(0.f) {}

    // pack returns { succeeded, rect id }. If not succeeded, id is irrelevant (but ideally should
    // be set to the maximum possible value).
    std::pair<bool, size_t> pack(const sf::Vector2f& rectSize) {
        // Does the rect fit at the current cursor position, on the current shelf?
        bool fitsOnCurrentShelf =
            (mCursorX + rectSize.x <= mBinSize.x) && (mShelfY + rectSize.y <= mBinSize.y);

        if (!fitsOnCurrentShelf) {
            // Doesn't fit here -- try dropping down to a brand-new shelf below this one.
            float newShelfY = mShelfY + mShelfMaxHeight;
            bool fitsOnNewShelf =
                (rectSize.x <= mBinSize.x) && (newShelfY + rectSize.y <= mBinSize.y);

            if (!fitsOnNewShelf) {
                // No room left anywhere in the bin for this rect.
                return {false, SIZE_MAX};
            }
            // Commit to the new shelf: reset the cursor and the shelf-height tracker.
            mShelfY = newShelfY;
            mCursorX = 0.0f;
            mShelfMaxHeight = 0.0f;
        }

        // Place the rect at the current cursor and remember its bounds for getBounds().
        sf::FloatRect placedBounds = sf::FloatRect({mCursorX, mShelfY}, rectSize);
        size_t newID = mPackedRects.size();
        mPackedRects.push_back(placedBounds);

        // Advance the cursor past the rect just placed, and track the shelf's tallest rect.
        mCursorX += rectSize.x;
        mShelfMaxHeight = std::max(mShelfMaxHeight, rectSize.y);

        return {true, newID};
    }

    // Returns the coordinates and size of a previously returned rectangle id.
    // Note that in this coordinate system, y points downwards from the origin at the top-left.
    sf::FloatRect getBounds(size_t rectID) const {
        assert(rectID < mPackedRects.size());
        return mPackedRects[rectID];
    }

private:
    sf::Vector2f mBinSize;  // Overall bin size
    float mShelfY;          // Top y of current shelf
    float mCursorX;         // Next x to place a rect at on current shelf
    float mShelfMaxHeight;  // Tallest rect placed on current shelf so far
    std::vector<sf::FloatRect> mPackedRects;
};

// Returns a new image which pads the given img with *pixels* pixels on each side.
// The colour of new border pixels are taken by sampling the closest valid pixel in the original
// image.
//
// NOTE: a production implementation might also support other edge wrapping behaviors, like cycling
// around to the other side of the texture. See https://learnopengl.com/Getting-started/Textures
// Our implementation corresponds to GL_CLAMP_TO_EDGE
inline sf::Image padImage(const sf::Image& img, uint pixels) {
    sf::Vector2u origSize = img.getSize();
    uint origWidth = origSize.x;
    uint origHeight = origSize.y;

    // The padded image grows by `pixels` on every side, so by 2x pixels on each axis.
    uint newWidth = origWidth + 2 * pixels;
    uint newHeight = origHeight + 2 * pixels;

    sf::Image result;
    result.resize({newWidth, newHeight});

    for (uint destY = 0; destY < newHeight; ++destY) {
        for (uint destX = 0; destX < newWidth; ++destX) {
            // Map this padded-image pixel back into the original image's coordinate space.
            int srcXSigned = int(destX) - int(pixels);
            int srcYSigned = int(destY) - int(pixels);

            // Clamp out-of-range coordinates to the nearest valid edge pixel (clamp-to-edge).
            uint srcX = uint(std::clamp(srcXSigned, 0, int(origWidth) - 1));
            uint srcY = uint(std::clamp(srcYSigned, 0, int(origHeight) - 1));

            sf::Color color = img.getPixel({srcX, srcY});
            result.setPixel({destX, destY}, color);
        }
    }

    return result;
}

using TextureID = size_t;

class TextureAtlas {
public:
    TextureAtlas(sf::Vector2u binSize, size_t padding)
        : mTexture(binSize), mPacker(sf::Vector2f(mTexture.getSize())), mPadding{padding} {}

    TextureID registerTexture(const std::string& filepath) {
        sf::Image original;
        bool loadOk = original.loadFromFile(filepath);
        assert(loadOk);

        // Pad the image so neighbouring atlas entries don't bleed into each other, then pack it.
        sf::Image padded = padImage(original, mPadding);
        sf::Vector2f paddedSize = sf::Vector2f(padded.getSize());

        std::pair<bool, size_t> packResult = mPacker.pack(paddedSize);
        bool packOk = packResult.first;
        size_t packedRectID = packResult.second;
        assert(packOk);

        // Write the padded pixels into the atlas texture at the position the packer chose.
        sf::FloatRect paddedBounds = mPacker.getBounds(packedRectID);
        sf::Vector2u paddedPosPixels = sf::Vector2u(paddedBounds.position);

        mTexture.update(padded, paddedPosPixels);

        // Remember the UNPADDED rect (inset by mPadding on each side) for later lookups.
        sf::Vector2u originalSize = original.getSize();
        sf::Vector2f unpaddedPos =
            paddedBounds.position + sf::Vector2f(float(mPadding), float(mPadding));
        sf::FloatRect unpaddedBounds = sf::FloatRect(unpaddedPos, sf::Vector2f(originalSize));

        TextureID newID = mUnpaddedPixelBounds.size();
        mUnpaddedPixelBounds.push_back(unpaddedBounds);
        return newID;
    }

    sf::FloatRect getBoundsUV(TextureID id) const {
        assert(id < mUnpaddedPixelBounds.size());
        sf::FloatRect pixelRect = mUnpaddedPixelBounds[id];

        sf::Vector2u atlasSize = mTexture.getSize();
        float atlasWidth = float(atlasSize.x);
        float atlasHeight = float(atlasSize.y);

        // Pixel space is y-down from the top; UV space is y-up from the bottom, so flip here.
        float pixelTop = pixelRect.position.y;
        float pixelBottom = pixelRect.position.y + pixelRect.size.y;

        // u maps directly -- the x axis points the same way in both spaces.
        float u0 = pixelRect.position.x / atlasWidth;
        float u1 = (pixelRect.position.x + pixelRect.size.x) / atlasWidth;

        float vTop = 1.0f - pixelTop / atlasHeight;
        float vBottom = 1.0f - pixelBottom / atlasHeight;

        return sf::FloatRect({u0, vBottom}, {u1 - u0, vTop - vBottom});
    }

    sf::Vector2u getSizePixels(TextureID id) const {
        assert(id < mUnpaddedPixelBounds.size());
        sf::FloatRect unpaddedRect = mUnpaddedPixelBounds[id];
        return sf::Vector2u(unpaddedRect.size);
    }

    const sf::Texture& getTexture() const { return mTexture; }

private:
    sf::Texture mTexture;
    RectPacker mPacker;
    size_t mPadding;
    std::vector<sf::FloatRect> mUnpaddedPixelBounds;
};

#endif  // TEXTURE_ATLAS_H