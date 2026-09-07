"""Exhaustive matching oracle, independent of GraphMini scheduling/codegen."""
import itertools


def count_matches(data, query, induced):
    n, k = len(data), len(query)
    automorphisms = sum(
        all(query[i][j] == query[p[i]][p[j]] for i in range(k) for j in range(k))
        for p in itertools.permutations(range(k))
    )
    embeddings = sum(
        all(
            data[p[i]][p[j]] == query[i][j]
            if induced else not query[i][j] or data[p[i]][p[j]]
            for i in range(k) for j in range(i)
        )
        for p in itertools.permutations(range(n), k)
    )
    assert embeddings % automorphisms == 0
    return embeddings // automorphisms


def matrix(n, edges):
    result = [[0] * n for _ in range(n)]
    for i, j in edges:
        result[i][j] = result[j][i] = 1
    return result
