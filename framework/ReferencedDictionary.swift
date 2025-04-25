
// This is a dictionary which is a class instead of struct. We do that so it can be used in contexts where we need to mutate the dictionary
// without mutating the struct containing. The main use case for this is in the remote process notification enums. In particular, we want to
// have the `.connected` state have an associated set of values which are the live notifiers, which can be mutated without modifying the state
// object itself as the only way for the dispatch source cancel handler is to capture it at creation time. Since it will be captured by value we
// need the by reference indirection for it to pickup notifier change registration.

// This pattern is not inherently concurrency safe, so it must be protected by some external mecanism such a serial dispatch queue.

class ReferencedDictionary<K : Hashable,V>: Collection  {
    subscript(position: Dictionary<K, V>.Index) -> Dictionary<K, V>.Element {
        //TODO: We should implement _read, but it ICEs the compiler and this is not perf sensitive
        get {
            return dictionary[position]
        }
    }
    subscript(key: K) -> V? {
        get {
            return dictionary[key]
        }
        set {
            dictionary[key] = newValue
        }
    }

    func index(after i: Dictionary<K, V>.Index) -> Dictionary<K, V>.Index {
        dictionary.index(after:i)
    }
    private var dictionary: Dictionary<K,V> = [:]
    typealias Element = Dictionary<K,V>.Element
    var startIndex:Dictionary<K,V>.Index {
        return dictionary.startIndex
    }
    var endIndex:Dictionary<K,V>.Index {
        return dictionary.endIndex
    }
    func removeAll() {
        dictionary.removeAll()
    }
    var isEmpty: Bool {
        return dictionary.isEmpty
    }
    @discardableResult
    func removeValue(forKey key: K) -> V? {
        dictionary.removeValue(forKey: key)
    }
}
