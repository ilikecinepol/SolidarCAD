export interface Release {
  version: string;
  channel: string;
  platform: string;
  downloadUrl: string;
  releaseUrl: string;
  releaseDate: string;
  size: string;
  sha256: string;
  notes: string[];
}
// Only publish verified metadata from the same GitHub Release asset.
export const currentRelease: Release = {
  version: "",
  channel: "MVP",
  platform: "Windows x64",
  downloadUrl: "",
  releaseUrl: "",
  releaseDate: "",
  size: "",
  sha256: "",
  notes: [],
};
export const githubUrl = "https://github.com/ilikecinepol/SolidarCAD";
export const releasesUrl = `${githubUrl}/releases`;
