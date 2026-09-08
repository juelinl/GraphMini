"""Count induced vertex subsets once, without a symmetry divisor or scheduler."""
import itertools


def count_induced_subsets(data, query):
    k = len(query)
    degrees = [sum(row) for row in query]

    def isomorphic(vertices):
        host_degrees = [sum(data[v][w] for w in vertices) for v in vertices]
        if sorted(host_degrees) != sorted(degrees):
            return False
        choices = [[i for i, degree in enumerate(host_degrees) if degree == degrees[q]]
                   for q in range(k)]
        order = sorted(range(k), key=lambda q: (len(choices[q]), -degrees[q]))
        assigned = {}
        used = set()

        def search(depth):
            if depth == k:
                return True
            q = order[depth]
            for candidate in choices[q]:
                if candidate in used:
                    continue
                if any(query[q][prior] != data[vertices[candidate]][vertices[value]]
                       for prior, value in assigned.items()):
                    continue
                assigned[q] = candidate
                used.add(candidate)
                if search(depth + 1):
                    return True
                used.remove(candidate)
                del assigned[q]
            return False

        return search(0)

    return sum(isomorphic(vertices) for vertices in itertools.combinations(range(len(data)), k))
