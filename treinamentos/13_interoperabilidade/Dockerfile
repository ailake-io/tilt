# Build a partir da raiz do projeto:
# docker build -f treinamentos/13_interoperabilidade/Dockerfile -t tilt-interop .
# docker run --rm tilt-interop

FROM python:3.12-slim

WORKDIR /workspace
COPY build/release/bin/tilt /opt/tilt/bin/tilt
COPY python /workspace/python
COPY treinamentos /workspace/treinamentos
COPY exemplos /workspace/exemplos

RUN chmod +x /opt/tilt/bin/tilt \
    && python -m pip install --no-cache-dir -e /workspace/python

ENV TILT_BIN=/opt/tilt/bin/tilt
ENV PYTHONPATH=/workspace/python

CMD ["python", "treinamentos/13_interoperabilidade/python_cliente.py"]
