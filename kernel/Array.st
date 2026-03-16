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
    | sz result i |
    sz := self size.
    result := '#('.
    i := 1.
    [i <= sz] whileTrue: [
        i > 1 ifTrue: [result := result, ' '].
        result := result, (self at: i) printString.
        i := i + 1].
    result := result, ')'.
    ^ result! !

!Array methodsFor: 'converting'!
asArray
    ^ self! !
