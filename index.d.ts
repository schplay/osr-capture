declare module "osr-capture" {
    /**
     * Read back an Electron OSR shared GPU texture into a tightly-packed BGRA CPU buffer, off the main thread.
     *
     * @param handle The 8-byte Buffer from the paint event's `texture.textureInfo.sharedTextureHandle`.
     * @param width  `texture.textureInfo.codedSize.width`.
     * @param height `texture.textureInfo.codedSize.height`.
     * @returns A Promise resolving to a BGRA buffer of `width * 4 * height` bytes.
     */
    export function readback(handle: Buffer, width: number, height: number): Promise<Buffer>
}
