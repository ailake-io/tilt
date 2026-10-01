# Política de compatibilidade

O beta usa versionamento semântico: `MAJOR.MINOR.PATCH`. O sufixo `-beta.N`
identifica uma prévia e pode conter mudanças incompatíveis antes de `1.0.0`.

Durante a série `0.2.x`:

- a sintaxe documentada, os nomes de builtins e o formato de diagnósticos
  estáveis serão preservados dentro de uma mesma versão menor;
- extensões experimentais podem mudar ou ser removidas, sempre registradas
  no changelog e na matriz de recursos;
- arquivos Parquet, Delta, Iceberg, JSON, CSV e manifests de registry gerados
  pelo Tilt continuam legíveis por versões posteriores dentro de `0.2.x`;
- a ABI C++ e os detalhes internos do runtime não são uma API pública; cada
  release deve ser recompilada para a plataforma alvo;
- CUDA, Metal, conectores remotos e bibliotecas carregadas por `dlopen` são
  recursos opcionais. A ausência deles deve manter o fallback CPU ou produzir
  um diagnóstico explícito, nunca alterar silenciosamente o resultado;
- mudanças de schema, de protocolo de catálogo e de políticas de segurança
  serão descritas na documentação de limitações antes de serem consideradas
  estáveis.

O suporte mínimo desta beta é CMake 3.20, compilador C++20 (GCC 11+ ou Clang
14+) e um sistema de 64 bits. Linux x86_64, macOS arm64/x86_64 e Windows x64
são as plataformas empacotadas oficialmente. Outras arquiteturas podem compilar, mas
não fazem parte da matriz de binários publicados.

Para atualizar, valide o checksum do artefato e execute a suíte de regressão
antes de promover um pipeline de produção. Para relatar uma quebra, inclua a
versão de `tilt versao`, sistema operacional, compilador e um arquivo `.tilt`
mínimo.
