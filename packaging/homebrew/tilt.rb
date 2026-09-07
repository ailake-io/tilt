# Fórmula Homebrew do Tilt.
#
# Uso em tap próprio (ailake-io/homebrew-tilt):
#   cp packaging/homebrew/tilt.rb <tap>/Formula/tilt.rb
#   brew install ailake-io/tap/tilt
#
# Atualização de versão: troque `url` e `sha256` (do tarball da Release
# `v*`) e a data em `version`. A sha256 do tarball é publicada como
# `.tar.gz.sha256` na Release do GitHub.
class Tilt < Formula
  desc "Linguagem de programação para dados e IA (dados, ML/DL, LLM/RAG, agentes, HTTP)"
  homepage "https://github.com/ailake-io/tilt"
  url "https://github.com/ailake-io/tilt/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "TROCAR_PELA_SHA256_DO_TARBALL_DA_RELEASE"
  license "MIT"

  depends_on "cmake" => :build

  # curl é usado em runtime para HTTP externo (LLM, S3, Qdrant, Iceberg REST).
  depends_on "curl"

  def install
    system "cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build", "--prefix", prefix
  end

  test do
    (testpath/"ola.tilt").write <<~TILT
      pipeline ola:
        passos:
          - imprimir "oi do brew"
    TILT
    assert_match "oi do brew", shell_output("#{bin}/tilt executar ola.tilt")
  end
end
