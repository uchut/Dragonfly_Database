# Proactor_threads 동적 할당

clients의 connection 병렬성이 낮을 때 Dragonfly가 Redis보다 낮은 throughput이 측정됨
- client Socket 수를 감지해 이에 맞춰 Proactor_threads를 1대1로 동적 생성/종료 하도록 구현
