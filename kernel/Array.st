"Array kernel methods.
 Primitives installed by bootstrap: at: (51), at:put: (52).
 size inherited from ArrayedCollection (prim 53).
 replaceFrom:to:with:startingAt: inherited from ArrayedCollection (prim 54).
 do:, collect:, select:, reject:, detect:ifNone:, includes:,
 first, last inherited from SequenceableCollection.
 Class-side with: constructors deferred — file-in does not support
 metaclass method installation."

!Array methodsFor: 'printing'!
printString
    | result |
    result := '#('.
    1 to: self size do: [:i |
        i > 1 ifTrue: [result := result, ' '].
        result := result, (self at: i) printString].
    result := result, ')'.
    ^ result! !

!Array methodsFor: 'converting'!
asArray
    ^ self! !
