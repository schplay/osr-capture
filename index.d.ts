declare module "osr-capture" {
    interface DmabufPlane {
        fd: number
        stride: number
        offset: number
        size: number
    }

    interface DmabufInfo {
        planes: DmabufPlane[]
        modifier: number | bigint
    }

    /**
     * Read back an Electron OSR shared texture into a tightly-packed BGRA CPU buffer, off the main thread.
     *
     * @param source Windows/macOS: the 8-byte Buffer from `texture.textureInfo.sharedTextureHandle`.
     *               Linux: `{ planes: texture.textureInfo.planes, modifier: texture.textureInfo.modifier }`.
     * @param width  `texture.textureInfo.codedSize.width`.
     * @param height `texture.textureInfo.codedSize.height`.
     * @returns A Promise resolving to a BGRA buffer of `width * 4 * height` bytes.
     */
    export function readback(source: Buffer | DmabufInfo, width: number, height: number): Promise<Buffer>
}
